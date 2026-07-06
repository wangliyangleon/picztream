#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include "cli/kitty/kitty.h"
#include "cli/term/cbreak_mode.h"
#include "cli/term/debug_log.h"
#include "cli/term/screen.h"
#include "core/api.h"

namespace {

void print_usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  pzt new <project_name> [folder_path]\n"
               "  pzt list\n"
               "  pzt open [project_name] [--debug]  (h/l 上一张/下一张,"
               "j/k 下一张/上一张未打标签,space 打标签,x 标记废片,g 筛选,"
               "r 应用/清除/新建/删除风格,r v 临时预览原图,"
               "q 退出;--debug 时在图片下方开一块区域滚动显示内部日志,默认"
               "不显示也不产生这些日志)\n"
               "  pzt archive <project_name>\n"
               "  pzt delete <project_name>\n"
               "  pzt rescan <project_name> [--no-prune]  (默认会清除磁盘上已消失的"
               "文件记录,连带清掉其标签;对着可能暂时没挂载完整的存储位置跑时,"
               "加 --no-prune 跳过清理)\n"
               "  pzt export <project_name> <tag_name> <output_folder> [--link]\n"
               "  pzt tag list <project_name>\n"
               "  pzt recipe list\n");
}

void print_tag_usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  pzt tag list <project_name>\n");
}

void print_recipe_usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  pzt recipe list\n"
               "  pzt recipe rename <preset>:<version_number> <new_name>\n"
               "  pzt recipe delete <preset>:<version_number>\n");
}

// 找不到项目时打印统一格式的错误提示。返回 nullopt 表示调用方应该直接
// return 1。
std::optional<pzt::core::ProjectId> resolve_project(const std::string& cmd,
                                                     const std::string& project_name) {
  auto id = pzt::core::find_project_by_name(project_name);
  if (!id) {
    std::fprintf(stderr, "%s: 找不到项目 '%s',用 pzt list 查看可用项目\n", cmd.c_str(),
                 project_name.c_str());
  }
  return id;
}

// 三面板布局(increment 6.4.2)用的一组小工具。全部走 write(fd, ...) 而不是
// fprintf/std::cout——render_rgba_via_tmpfile/clear_placement 内部也是直接
// write() 到同一个 fd,如果光标定位这些文字改用带缓冲的 stdio 输出,两条路
// 径谁先真正落地到终端就不可控了,布局又会跟 6.4.1 那次一样错位。
void write_stdout(const std::string& s) { write(STDOUT_FILENO, s.data(), s.size()); }

void move_cursor(int row, int col) {
  write_stdout("\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H");
}

// 判断一个 Unicode 码点在终端里是否按"宽字符"(占 2 列)显示——覆盖这个
// 项目实际会用到的范围(CJK 统一表意文字、全角标点、假名、谚文等),不追
// 求覆盖 Unicode East Asian Width 规范的每一个区间。
bool is_wide_codepoint(char32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) ||    // Hangul Jamo
         (cp >= 0x2E80 && cp <= 0xA4CF) ||    // CJK 部首/符号/假名/统一表意文字等
         (cp >= 0xAC00 && cp <= 0xD7A3) ||    // Hangul 音节
         (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK 兼容表意文字
         (cp >= 0xFF00 && cp <= 0xFF60) ||    // 全角字符
         (cp >= 0xFFE0 && cp <= 0xFFE6) ||
         (cp >= 0x20000 && cp <= 0x3FFFD);    // CJK 扩展区(增补平面)
}

// 解码 s[pos] 起的一个 UTF-8 字符,返回(码点, 字节数)。非法/截断的字节序
// 列当成 1 字节宽字符处理,不崩溃、不越界。
std::pair<char32_t, int> decode_utf8_at(const std::string& s, std::size_t pos) {
  unsigned char c0 = static_cast<unsigned char>(s[pos]);
  if (c0 < 0x80) return {c0, 1};
  int len;
  char32_t cp;
  if ((c0 & 0xE0) == 0xC0) {
    len = 2;
    cp = c0 & 0x1F;
  } else if ((c0 & 0xF0) == 0xE0) {
    len = 3;
    cp = c0 & 0x0F;
  } else if ((c0 & 0xF8) == 0xF0) {
    len = 4;
    cp = c0 & 0x07;
  } else {
    return {c0, 1};
  }
  if (pos + static_cast<std::size_t>(len) > s.size()) return {c0, 1};
  for (int i = 1; i < len; ++i) {
    unsigned char c = static_cast<unsigned char>(s[pos + static_cast<std::size_t>(i)]);
    if ((c & 0xC0) != 0x80) return {c0, 1};
    cp = (cp << 6) | (c & 0x3F);
  }
  return {cp, len};
}

// 按终端实际显示宽度截断到 max_width 列以内——中文等宽字符占 2 列,不是简
// 单按字节数近似(6.4.2/6.4.3 早期版本的简化,extended 6.4.3 的 banner 文
// 案变长后暴露出这个近似会把不该截断的文字截断掉,这次改成按码点正确计
// 算)。放不下最后一个字符时就地停止,不做"半个宽字符"截断,也不强行拼
// "..."(这里是终端 UI 元素,不是给人读的完整句子,截断了就是没画完,不需
// 要额外标记)。
std::string truncate_text(const std::string& s, std::size_t max_width) {
  std::string out;
  std::size_t display_w = 0;
  std::size_t pos = 0;
  while (pos < s.size()) {
    auto [cp, len] = decode_utf8_at(s, pos);
    std::size_t w = is_wide_codepoint(cp) ? 2 : 1;
    if (display_w + w > max_width) break;
    out += s.substr(pos, static_cast<std::size_t>(len));
    display_w += w;
    pos += static_cast<std::size_t>(len);
  }
  return out;
}

std::string format_size(std::int64_t bytes) {
  double v = static_cast<double>(bytes);
  const char* units[] = {"B", "KB", "MB", "GB"};
  int i = 0;
  while (v >= 1024.0 && i < 3) {
    v /= 1024.0;
    ++i;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f%s", v, units[i]);
  return buf;
}

// increment 6.6:导出路径展开 `~`/`~/...`。标准子命令的路径参数从 argv 来,
// shell 早就在我们看到之前展开过 `~`(除非用户自己加引号);但 `g e` 走的
// 是 read_text_line 直接读键盘字节,完全绕过 shell,不会有人替我们展开——
// 真机验证发现不展开的话 `~` 会被字面创建成一个真的叫 `~` 的目录,"导出成
// 功"但成功到了一个用户没想到的地方。两个入口(cmd_export 和
// handle_g_export_flow)统一走这个函数,行为保持一致。只处理 `~` 和
// `~/...` 这两种形式,不处理 `~user`——M0 单用户场景不需要。
std::string expand_home_path(const std::string& path) {
  if (path != "~" && path.rfind("~/", 0) != 0) return path;
  const char* home = std::getenv("HOME");
  if (!home || home[0] == '\0') return path;  // 环境异常,原样返回,不猜测
  return path == "~" ? std::string(home) : std::string(home) + path.substr(1);
}

std::size_t display_width(const std::string& s) {
  std::size_t w = 0;
  std::size_t pos = 0;
  while (pos < s.size()) {
    auto [cp, len] = decode_utf8_at(s, pos);
    w += is_wide_codepoint(cp) ? 2 : 1;
    pos += static_cast<std::size_t>(len);
  }
  return w;
}

// 截断/补空格到固定的显示宽度——按边框内容区写字的地方都要用这个,而不是
// 裸写字符串,否则这一帧内容比上一帧短的时候,会在文字和右边框之间留下
// 上一帧的残留字符。
std::string pad_to(const std::string& s, std::size_t width) {
  std::string t = truncate_text(s, width);
  std::size_t w = display_width(t);
  if (w < width) t += std::string(width - w, ' ');
  return t;
}

// 画一条横线(边框顶/底/分隔线用):起止两端用 left_char/right_char,如果
// mid_offset >= 0,在这一列(相对 start_col 的偏移,0 是最左边框那一列)插
// 入 mid_char(跟竖直分隔线交汇的地方),其余位置用 "─" 填满。宽度按显示列
// 数算,不是字节数——box-drawing 字符在 UTF-8 里是多字节,这里始终整存整
// 取一个字符,不做字节级切片。
void draw_hline(int row, int start_col, int width, const std::string& left_char,
                 const std::string& right_char, int mid_offset = -1,
                 const std::string& mid_char = "") {
  move_cursor(row, start_col);
  std::string line = left_char;
  for (int i = 1; i < width - 1; ++i) {
    line += (i == mid_offset) ? mid_char : "─";  // ─
  }
  line += right_char;
  write_stdout(line);
}

// 画一行内容左右两侧的边框竖线;mid_offset >= 0 时额外在中间(图片区/信息
// 栏分隔处)也画一条。内容本身仍然由调用方单独 move_cursor+write_stdout。
void draw_vlines(int row, int start_col, int width, int mid_offset = -1) {
  move_cursor(row, start_col);
  write_stdout("│");  // │
  if (mid_offset >= 0) {
    move_cursor(row, start_col + mid_offset);
    write_stdout("│");
  }
  move_cursor(row, start_col + width - 1);
  write_stdout("│");
}

// increment 6.4.4:space 快速标签菜单。见 docs/M0_Eng_Design.md 对应小节。

// `list_tags` 是按名字字母序排的(给 `pzt tag list` 这类展示用)——数字键
// 要按标签创建顺序(tag id 升序)固定,不然新建一个名字靠前的标签会让所有
// 已有标签的数字悄悄错位。不改 `list_tags` 本身,这里对结果客户端重排序。
// 只有 1-9 有数字键,超过的这次不处理。increment 6.4.5:系统标签("废片")
// 固定占硬编码的 `0`,不参与这个动态的 1-9 序列,先过滤掉。
std::vector<pzt::core::TagSummary> tags_for_menu(pzt::core::ProjectId project_id) {
  auto tags = pzt::core::list_tags(project_id);
  tags.erase(std::remove_if(tags.begin(), tags.end(), [](const auto& t) { return t.is_system; }),
             tags.end());
  std::sort(tags.begin(), tags.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  if (tags.size() > 9) tags.resize(9);
  return tags;
}

// 两层菜单(选标签、cap 超限选替换对象)都只需要读一个字节就能得出最终结
// 果,不需要为"取消"单独过滤——EOF/出错和 Esc(0x1B)一样当取消处理。
char read_one_byte() {
  char c = 0;
  ssize_t n = read(STDIN_FILENO, &c, 1);
  return n <= 0 ? 0x1B : c;
}

// --debug 模式下,debug 面板的内容来自后台 prefetch 线程往 stderr 打的日
// 志,不依附于任何按键——主循环原本完全阻塞在 read() 上,刚 open 一个项目
// 时,当前这张图的解码日志还没写出来就已经画完这一帧,debug 面板要等用户
// 真的按下一个键触发下一次整屏重绘才会显示最新内容。这里用 poll() 代替阻
// 塞 read() 定期超时,超时就让外层循环重画一帧(不当成任何按键处理),debug
// 面板就能在没有导航操作的情况下也跟上后台日志。只在 debug_mode 时启用,
// 不开 --debug 时没有这个刷新的必要,阻塞 read 不多余地占用 CPU。
bool stdin_ready(int timeout_ms) {
  struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
  int ret = poll(&pfd, 1, timeout_ms);
  return ret > 0;
}

// 给定一个 UTF-8 码点的起始字节,返回后面还需要几个续字节才能凑成一个完
// 整码点(0-3)。非法起始字节按 0 处理,当成单字节字符,不阻塞输入。
int utf8_continuation_bytes(unsigned char lead) {
  if (lead < 0x80) return 0;
  if ((lead & 0xE0) == 0xC0) return 1;
  if ((lead & 0xF0) == 0xE0) return 2;
  if ((lead & 0xF8) == 0xF0) return 3;
  return 0;
}

// increment 6.4.4.5:从 banner 那一行读一整行 UTF-8 文本(新建标签要输入
// 名称,这是这个项目第一次需要真正的多字节文本输入,其它菜单都只读一个字
// 节)。ECHO 在 cbreak 模式下是关的,要手动重画"提示 + 已输入内容"才能让
// 用户看到自己打了什么——但不是每个字节都重画,攒够一个完整的 UTF-8 码点
// 才画,避免把不完整的多字节序列写到终端上:这样不用赌终端自己的 UTF-8
// 解码器能不能扛住半个字符,也不用赌一个按键的几个字节到达得够快、人眼
// 看不出中间状态,多写这几行代码换来不用赌。Esc/EOF 返回 nullopt(整个流
// 程取消);Enter(`\r` 或 `\n`,两个都接受——cbreak 模式没动 `ICRNL`,具体
// 哪个字节真正到达不能只看代码确定)返回目前的缓冲区内容(可能是空字符
// 串,调用方决定空值是否合法)。退格(DEL `0x7F` 或 BS `0x08`,两个都处
// 理)整个删掉最后一个 UTF-8 码点,不是只删一个字节——因为 `ICANON` 关了,
// 内核不会自动处理退格。
std::optional<std::string> read_text_line(const std::string& prompt, int banner_row,
                                           int start_col, int content_cols) {
  std::string buffer;
  int pending_needed = 0;  // 还差几个续字节才能凑成当前码点

  auto redraw = [&] {
    move_cursor(banner_row, start_col + 1);
    write_stdout(pad_to(prompt + buffer, content_cols));
  };
  redraw();

  while (true) {
    char c = read_one_byte();
    if (c == 0x1B) return std::nullopt;
    if (c == '\r' || c == '\n') return buffer;
    if (c == 0x7F || c == 0x08) {
      if (!buffer.empty()) {
        std::size_t pos = buffer.size() - 1;
        while (pos > 0 && (static_cast<unsigned char>(buffer[pos]) & 0xC0) == 0x80) --pos;
        buffer.erase(pos);
      }
      pending_needed = 0;
      redraw();
      continue;
    }
    if (static_cast<unsigned char>(c) < 0x20) continue;  // 其它控制字节,忽略

    buffer += c;
    if (pending_needed > 0) {
      --pending_needed;
    } else {
      pending_needed = utf8_continuation_bytes(static_cast<unsigned char>(c));
    }
    if (pending_needed == 0) redraw();
  }
}

// cap 超限时,在 banner 那一行显示已有条目、读一个键选替换对象或取消。
std::string handle_cap_replace_submenu(pzt::core::TagId tag_id, pzt::core::ImageId new_image_id,
                                        const pzt::core::CapExceededInfo& cap_info, int banner_row,
                                        int start_col, int content_cols) {
  if (cap_info.existing_entries.empty()) {
    return " 标签上限为 0,无法添加 ";  // cap=0 的极端配置,没有可替换的条目
  }
  std::size_t shown = std::min<std::size_t>(cap_info.existing_entries.size(), 9);

  std::string line = " 已满(" + std::to_string(cap_info.cap) + "):";
  for (std::size_t i = 0; i < shown; ++i) {
    if (i > 0) line += "  ";
    line += std::to_string(i + 1) + ":" + cap_info.existing_entries[i].file_name;
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  if (c < '1' || c > static_cast<char>('0' + shown)) return "";  // 取消,静默

  const auto& old_entry = cap_info.existing_entries[static_cast<std::size_t>(c - '1')];
  auto result = pzt::core::replace_tag_entry(tag_id, old_entry.image_id, new_image_id);
  if (!result.ok()) return " 替换失败,请重试 ";  // 防御性,理论上不应该发生
  return " 已替换 '" + old_entry.file_name + "' ";
}

// increment 6.4.4.5:space - 摘除标签。复用跟"加标签"完全相同的列表/编
// 号(id 升序,不是"只显示当前图片有的标签")——`tags_for_menu` 客户端重
// 排序就是为了让"数字 N 永远对应同一个标签"这条肌肉记忆成立,摘除菜单换
// 一套编号的话,同一个数字在两个菜单里意味着不同标签,反而更容易按错。选
// 中一个当前图片本来就没有的标签,`remove_tag` 本来就是幂等的,不需要额
// 外校验或过滤。菜单文案前缀"摘除:"跟加标签菜单区分开,给一个明确的视觉
// 提示"现在是摘除模式"。
std::string handle_remove_tag_submenu(const std::vector<pzt::core::TagSummary>& tags,
                                       pzt::core::TagId reject_tag_id, pzt::core::ImageId image_id,
                                       int banner_row, int start_col, int content_cols) {
  std::string line = " 摘除:0:废片";
  for (std::size_t i = 0; i < tags.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + tags[i].name;
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  pzt::core::TagId tag_id;
  if (c == '0') {
    tag_id = reject_tag_id;
  } else if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
    tag_id = tags[static_cast<std::size_t>(c - '1')].id;
  } else {
    return "";  // 取消,静默
  }

  auto result = pzt::core::remove_tag(image_id, tag_id);
  if (!result.ok()) return " 摘标签失败,请重试 ";  // 防御性,理论上不应该发生
  return "";  // 静默成功,信息栏下一帧自然显示摘除后的结果
}

// increment 6.4.4.5:space c 新建标签。跟其它菜单不同,这里需要读入一个
// 完整的标签名(多字符文本),用 read_text_line;上限和是否排序两个后续问
// 题分别用文本输入/单字节是否处理。
std::string handle_create_tag_flow(pzt::core::ProjectId project_id, int banner_row, int start_col,
                                    int content_cols) {
  auto name = read_text_line(" 新标签名称: ", banner_row, start_col, content_cols);
  if (!name) return "";  // Esc,静默取消
  if (name->empty()) {
    // 空回车不是 Esc——用户确实按了键,不能像 Esc 一样什么反馈都没有。
    return " 标签名不能为空,已取消 ";
  }

  auto cap_text =
      read_text_line(" 上限数量(直接 Enter = 不限): ", banner_row, start_col, content_cols);
  if (!cap_text) return "";  // Esc 中止整个流程,即便名字已经输完了
  std::optional<std::int64_t> cap;
  if (!cap_text->empty()) {
    // 尝试整体解析成正整数;解析不完整/非数字/<=0 都当"不限"处理,不重新
    // 提示、不阻塞重试——cap 是低风险的元数据(填错了大不了删掉重建),这
    // 个菜单一贯的风格是不做校验重试循环。
    try {
      std::size_t consumed = 0;
      long long parsed = std::stoll(*cap_text, &consumed);
      if (consumed == cap_text->size() && parsed > 0) {
        cap = parsed;
      }
    } catch (const std::exception&) {
      // 解析失败,cap 保持 nullopt(不限)
    }
  }

  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(" 是否需要按顺序排列(用于朋友圈九宫格等,直接 Enter = 否): "
                      "y 是 / 其它键 = 否 ",
                      content_cols));
  char c = read_one_byte();
  if (c == 0x1B) return "";  // Esc 在这一步依然中止整个流程
  bool is_ordered = (c == 'y' || c == 'Y');  // 其它任何键(包括裸回车)都算"否"

  auto result = pzt::core::create_tag(project_id, *name, cap, is_ordered);
  if (!result.ok()) return " 标签名 '" + *name + "' 已存在,未创建 ";
  return " 已创建标签 '" + *name + "' ";
}

// increment 6.4.4.5:space d 删除标签定义本身(项目级、级联清除所有图片
// 与它的关联),不是"摘掉当前图片的标签"。只列非系统标签——系统标签(目
// 前只有还没落地的"废片")不能删除,从一开始就不给选,而不是选了之后再拒
// 绝。比菜单里其它操作多一次按键确认("y" 才执行),因为这是这个菜单里破
// 坏性最强、唯一项目级且不可逆的操作,但没有重到 `pzt delete` 那种"重新打
// 一遍项目名"的程度——那种量级的确认是给独立执行的破坏性命令用的,跟这
// 个强调"不打断心流"的菜单体系不搭。
std::string handle_delete_tag_submenu(const std::vector<pzt::core::TagSummary>& tags,
                                       int banner_row, int start_col, int content_cols) {
  std::vector<pzt::core::TagSummary> deletable;
  for (const auto& t : tags) {
    if (!t.is_system) deletable.push_back(t);
  }
  if (deletable.empty()) {
    return " 没有可删除的标签 ";  // 不阻塞读键,跟"项目没有标签"同样的理由
  }

  std::string line = " 删除:";
  for (std::size_t i = 0; i < deletable.size(); ++i) {
    if (i > 0) line += "  ";
    line += std::to_string(i + 1) + ":" + deletable[i].name + "(" +
            std::to_string(deletable[i].tagged_count) + "张)";
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  if (c < '1' || c > static_cast<char>('0' + deletable.size())) return "";  // 取消,静默
  const auto& chosen = deletable[static_cast<std::size_t>(c - '1')];

  std::string confirm = " 确定删除标签 '" + chosen.name + "'(" +
                         std::to_string(chosen.tagged_count) + " 张关联)?此操作不可撤销。"
                         "y 确认 / 其它键取消 ";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(confirm, content_cols));
  char yn = read_one_byte();
  if (yn != 'y' && yn != 'Y') return "";  // 取消,静默

  auto result = pzt::core::delete_tag(chosen.id);
  if (!result.ok()) return " 删除失败,请重试 ";  // 防御性,理论上不应该发生
  return " 已删除标签 '" + chosen.name + "' ";
}

// increment 6.4.5:数字加标签的分支和 `x` 快捷键共用同一段结果处理逻
// 辑,避免"处理 add_tag 结果、cap 超限转 handle_cap_replace_submenu"这
// 段逻辑抄两遍。`x` 的 cap 超限分支结构上不可能被真正触发(`废片` 的 cap
// 恒为空),复用这段逻辑单纯是不想把同一段代码写两份,不是专门防御性设计。
std::string handle_add_tag_result(pzt::core::TagId tag_id, pzt::core::ImageId image_id,
                                   int banner_row, int start_col, int content_cols) {
  auto result = pzt::core::add_tag(image_id, tag_id);
  if (result.ok()) return "";  // 静默成功,信息栏下一帧自然显示新标签

  const auto& err = result.error();
  if (err.kind == pzt::core::AddTagFailureKind::CapExceeded) {
    return handle_cap_replace_submenu(tag_id, image_id, *err.cap_info, banner_row, start_col,
                                       content_cols);
  }
  // TagNotFound/ImageNotFound/ProjectMismatch:tag_id/image_id 都来自刚查
  // 出来的数据,理论上不会发生,防御性处理而不是假设不可能。
  return " 打标签失败,请重试 ";
}

// space 键的入口:在 banner 那一行显示可选标签、读一个键选标签或取消,
// cap 超限时转入 handle_cap_replace_submenu;`-`/`c`/`d` 分别转入摘除/新
// 建/删除标签定义。`0:废片` 是硬编码的固定选项,不占用 `tags_for_menu` 的
// 动态 1-9 序列——`废片` 从 pzt new 起就保证存在,不再有"这次按 space 完
// 全没有东西可选"的情况,不管动态列表是不是空都无条件阻塞读一个键。
std::string handle_space_key(pzt::core::ProjectId project_id, pzt::core::TagId reject_tag_id,
                              pzt::core::ImageId image_id, int banner_row, int start_col,
                              int content_cols) {
  auto tags = tags_for_menu(project_id);  // 只查一次,加/摘/删三个分支共用

  std::string line = " 0:废片";
  for (std::size_t i = 0; i < tags.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + tags[i].name;
    if (tags[i].cap) {
      line += "(" + std::to_string(tags[i].tagged_count) + "/" + std::to_string(*tags[i].cap) + ")";
    }
  }
  line += "  c:新建  d:删除  -:摘除  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  if (c == 'c') return handle_create_tag_flow(project_id, banner_row, start_col, content_cols);
  if (c == '-') {
    return handle_remove_tag_submenu(tags, reject_tag_id, image_id, banner_row, start_col,
                                      content_cols);
  }
  if (c == 'd') return handle_delete_tag_submenu(tags, banner_row, start_col, content_cols);
  if (c == '0') {
    return handle_add_tag_result(reject_tag_id, image_id, banner_row, start_col, content_cols);
  }
  if (c < '1' || c > static_cast<char>('0' + tags.size())) return "";  // 取消,静默

  const auto& chosen = tags[static_cast<std::size_t>(c - '1')];
  return handle_add_tag_result(chosen.id, image_id, banner_row, start_col, content_cols);
}

// increment 6.4.6:g + 数字切换到只浏览某个标签下图片的筛选视图,g + g
// 清除筛选。落地这个决定需要改 cmd_open 自己的 images/current_id/
// prefetch——这几个是 cmd_open 函数作用域内的局部变量,不像其它 handle_*
// 那样能靠传值参数完成整个动作,所以这里只返回"意图",不直接执行。
// increment 6.6:g + e 导出,跟前两者不同——它是个完全自包含的动作(选标
// 签 + 读路径 + 调 export_tag),不需要 cmd_open 再回头改 images/
// current_id,所以直接在这里(连同 handle_g_export_flow)把它执行完,用
// `Handled` 携带结果文案返回,风格上更接近 handle_space_key 那种"直接返回
// 状态提示"的做法,而不是 ApplyFilter/ClearFilter 那种"只返回意图"的做法。
enum class GKeyAction { Cancel, ClearFilter, ApplyFilter, Handled };

struct GKeyDecision {
  GKeyAction action = GKeyAction::Cancel;
  pzt::core::TagId tag_id{};  // 只有 action == ApplyFilter 时有意义
  std::string tag_name;       // 同上,顺便带出来给信息栏筛选提示用,不用每
                               // 帧再查一次 tags_for_menu 只为了显示名字
  std::string status;         // 只有 action == Handled 时有意义
};

// g + e 之后:选导出目标标签(数字编号同 g 菜单,或者当前处于筛选视图时按
// `e` 表示"就导出这个筛选标签",省一次选择)、读目标路径、调用
// export_tag。固定用 LinkMode::Copy——软链场景用独立的 `pzt export
// --link`,这个快捷方式不做模式切换,见 docs/M0_Eng_Design.md increment
// 6.6 的说明。空路径不是 Esc,得给个反馈而不是静默(跟 handle_create_tag_
// flow 空标签名的处理一致);Esc 在任一步都中止整个流程,静默。
std::string handle_g_export_flow(pzt::core::TagId reject_tag_id,
                                  const std::vector<pzt::core::TagSummary>& tags,
                                  std::optional<pzt::core::TagId> active_filter_tag_id,
                                  const std::string& active_filter_tag_name, int banner_row,
                                  int start_col, int content_cols) {
  std::string line = " 导出:";
  if (active_filter_tag_id) line += "e:当前筛选(" + active_filter_tag_name + ")  ";
  line += "0:废片";
  for (std::size_t i = 0; i < tags.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + tags[i].name;
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  pzt::core::TagId target_id;
  std::string target_name;
  if (c == 'e' && active_filter_tag_id) {
    target_id = *active_filter_tag_id;
    target_name = active_filter_tag_name;
  } else if (c == '0') {
    target_id = reject_tag_id;
    target_name = "废片";
  } else if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
    const auto& t = tags[static_cast<std::size_t>(c - '1')];
    target_id = t.id;
    target_name = t.name;
  } else {
    return "";  // 取消,静默(含 `e` 但没有筛选生效的情形)
  }

  auto path = read_text_line(" 导出到: ", banner_row, start_col, content_cols);
  if (!path) return "";  // Esc,静默取消
  if (path->empty()) return " 导出路径不能为空,已取消 ";
  std::string resolved_path = expand_home_path(*path);

  auto result = pzt::core::export_tag(target_id, resolved_path, pzt::core::LinkMode::Copy);
  if (!result.ok()) {
    if (result.error() == pzt::core::ExportTagError::IoError) {
      return " 导出目标 '" + resolved_path + "' 无法写入(权限不足或路径被占用) ";
    }
    return " 导出失败,请重试 ";  // TagNotFound:防御性,理论上不应该发生
  }

  const auto& r = result.value();
  if (r.exported_count == 0 && r.skipped.empty()) {
    return " 标签 '" + target_name + "' 下没有图片,未导出 ";
  }
  std::string status = " 已导出 " + std::to_string(r.exported_count) + " 张 '" + target_name +
                        "' 到 '" + resolved_path + "'";
  if (r.created_output_folder) status += "(目录不存在,已新建)";
  if (!r.skipped.empty()) {
    status += "(跳过 " + std::to_string(r.skipped.size()) + " 张)";
  }
  status += " ";
  return status;
}

GKeyDecision handle_g_key_prompt(pzt::core::TagId reject_tag_id,
                                  const std::vector<pzt::core::TagSummary>& tags,
                                  std::optional<pzt::core::TagId> active_filter_tag_id,
                                  const std::string& active_filter_tag_name, int banner_row,
                                  int start_col, int content_cols) {
  std::string line = " g:清除筛选  e:导出  0:废片";
  for (std::size_t i = 0; i < tags.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + tags[i].name;
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  if (c == 'g') return {GKeyAction::ClearFilter, {}, "", ""};
  if (c == 'e') {
    std::string status = handle_g_export_flow(reject_tag_id, tags, active_filter_tag_id,
                                               active_filter_tag_name, banner_row, start_col,
                                               content_cols);
    return {GKeyAction::Handled, {}, "", status};
  }
  if (c == '0') return {GKeyAction::ApplyFilter, reject_tag_id, "废片", ""};
  if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
    const auto& t = tags[static_cast<std::size_t>(c - '1')];
    return {GKeyAction::ApplyFilter, t.id, t.name, ""};
  }
  return {GKeyAction::Cancel, {}, "", ""};  // 取消,静默,跟其它菜单一致
}

// 切换浏览池子(应用筛选/清除筛选)后 current_id 该是谁:能留在原地就留在
// 原地(原来那张图还在新池子里),留不住就退回列表头。两个方向复用同一条
// 规则,不为"进筛选"和"出筛选"分别定义两套语义。
pzt::core::ImageId resolve_current_after_switch(const std::vector<pzt::core::ImageRef>& new_images,
                                                 pzt::core::ImageId desired) {
  for (const auto& ref : new_images) {
    if (ref.id == desired) return desired;
  }
  return new_images.front().id;
}

// increment 6:`r` 前缀键完整交互。预设列表不需要像 tags_for_menu 那样过
// 滤 is_system——目前所有预设(包括 Origin)都是 is_system,但这里没有"用
// 户自建预设"这个对立面需要排除,直接按创建顺序编号 1-9。跟废片固定占 0
// 号位不同,Origin 没有固定编号,它就是 list_presets() 里排第一的普通一
// 项——`r` + `0`/`r` + `r`(清除)是完全独立于预设列表的快捷路径,直接把
// recipe_id 设成 NULL,不经过"选中 Origin 预设"这条路,两者对"没有风格"
// 这件事产出相同效果但走的是不同路径,见 core/recipe/recipe.h
// ensure_default_presets 的说明。
std::vector<pzt::core::PresetSummary> presets_for_menu() {
  auto presets = pzt::core::list_presets();
  if (presets.size() > 9) presets.resize(9);
  return presets;
}

// apply/create/delete 三个流程都要"选一个预设"，这是第三处需要这个交互
// 的地方(前两处是 cli 调试命令时代的 find_preset_by_name，这次是真正的
// 交互式菜单)，抽成共用函数。
std::optional<pzt::core::PresetSummary> handle_pick_preset_prompt(int banner_row, int start_col,
                                                                    int content_cols) {
  auto presets = presets_for_menu();
  std::string line = " 选预设:";
  for (std::size_t i = 0; i < presets.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + presets[i].name;
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  if (c < '1' || c > static_cast<char>('0' + presets.size())) return std::nullopt;  // 取消,静默
  return presets[static_cast<std::size_t>(c - '1')];
}

// 已经选定一个预设之后，第二层选"这个预设的中性状态(0)"还是"某个已保存
// 的 version(1-9)"——只列未软删除的 version，编号规则跟 pzt recipe
// list/rename/delete 的寻址编号一致(排除已删除的、按 id 升序排位)。
std::optional<pzt::core::RecipeId> handle_pick_version_to_apply_prompt(
    const pzt::core::PresetSummary& preset, int banner_row, int start_col, int content_cols) {
  auto all_versions = pzt::core::list_versions(preset.id);
  std::vector<pzt::core::VersionSummary> live;
  for (const auto& v : all_versions) {
    if (!v.deleted) live.push_back(v);
  }
  if (live.size() > 9) live.resize(9);

  std::string line = " " + preset.name + ":  0:预设本身";
  for (std::size_t i = 0; i < live.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + live[i].name.value_or("(未命名)");
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  if (c == '0') return preset.id;
  if (c >= '1' && c <= static_cast<char>('0' + live.size())) {
    return live[static_cast<std::size_t>(c - '1')].id;
  }
  return std::nullopt;  // 取消,静默
}

// 删除流程的第二层选择：跟应用流程共用同一份"这个预设下未删除的
// version"列表，但不提供"0:预设本身"这个选项——预设不可删除，从一开始
// 就不给选，不是"选了之后拒绝"。
std::string handle_pick_version_to_delete_prompt(const pzt::core::PresetSummary& preset,
                                                  int banner_row, int start_col,
                                                  int content_cols) {
  auto all_versions = pzt::core::list_versions(preset.id);
  std::vector<pzt::core::VersionSummary> live;
  for (const auto& v : all_versions) {
    if (!v.deleted) live.push_back(v);
  }
  if (live.empty()) {
    return " '" + preset.name + "' 下没有可删除的 version ";  // 不阻塞读键,跟标签同款理由
  }
  if (live.size() > 9) live.resize(9);

  std::string line = " 删除(" + preset.name + "):";
  for (std::size_t i = 0; i < live.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + live[i].name.value_or("(未命名)");
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  if (c < '1' || c > static_cast<char>('0' + live.size())) return "";  // 取消,静默

  const auto& chosen = live[static_cast<std::size_t>(c - '1')];
  // 软删除:不影响已经引用这个 version 的图片渲染，只是从这个菜单里消
  // 失。跟 handle_delete_tag_submenu 的硬删除不同，这里不加一道额外的
  // y/N 二次确认——标签删除是级联清掉所有图片关联、不可逆的项目级操
  // 作，这里只是把它从"可选列表"里隐藏，风险量级不一样，不需要同等重量
  // 的确认仪式。
  auto result = pzt::core::delete_version(chosen.id);
  if (!result.ok()) return " 删除失败,请重试 ";  // 防御性,理论上不应该发生
  return " 已删除 '" + chosen.name.value_or("(未命名)") + "' ";
}

// increment 6.2:`r c` 交互式创建新 version——选一个基础预设、依次读高
// 光/暗光/白平衡红/蓝几个数值、可选的名字，对齐 handle_create_tag_flow
// 的多步骤读取风格。数值解析失败/留空都当 0 处理，不重新提示、不阻塞重
// 试——这几个参数是低风险的元数据，填错了大不了删掉重建，跟标签 cap 解
// 析失败时的处理哲学一致。创建之后不会自动应用到当前图片，对齐
// `space c` 建标签之后也不会自动打到当前图片上这个既有约定。
std::string handle_r_create_flow(int banner_row, int start_col, int content_cols) {
  auto preset = handle_pick_preset_prompt(banner_row, start_col, content_cols);
  if (!preset) return "";  // 取消,静默

  auto parse_double_or_zero = [](const std::optional<std::string>& s) -> double {
    if (!s || s->empty()) return 0.0;
    try {
      std::size_t consumed = 0;
      double v = std::stod(*s, &consumed);
      if (consumed == s->size()) return v;
    } catch (const std::exception&) {
      // 解析失败,落到下面的 0.0
    }
    return 0.0;
  };

  auto highlights_text =
      read_text_line(" 高光(直接 Enter = 0): ", banner_row, start_col, content_cols);
  if (!highlights_text) return "";  // Esc 中止整个流程
  auto shadows_text = read_text_line(" 暗光(直接 Enter = 0): ", banner_row, start_col, content_cols);
  if (!shadows_text) return "";
  auto wb_r_text =
      read_text_line(" 白平衡-红(直接 Enter = 0): ", banner_row, start_col, content_cols);
  if (!wb_r_text) return "";
  auto wb_b_text =
      read_text_line(" 白平衡-蓝(直接 Enter = 0): ", banner_row, start_col, content_cols);
  if (!wb_b_text) return "";
  auto name_text =
      read_text_line(" 名称(可选,直接 Enter = 不设置): ", banner_row, start_col, content_cols);
  if (!name_text) return "";

  pzt::core::VersionParams params;
  params.highlights = parse_double_or_zero(highlights_text);
  params.shadows = parse_double_or_zero(shadows_text);
  params.wb_shift_r = parse_double_or_zero(wb_r_text);
  params.wb_shift_b = parse_double_or_zero(wb_b_text);
  std::optional<std::string> name = name_text->empty() ? std::nullopt : name_text;

  auto result = pzt::core::create_version(preset->id, name, params);
  if (!result.ok()) return " 创建失败,请重试 ";  // 防御性,理论上不应该发生(预设一定存在)
  return " 已在 '" + preset->name + "' 下创建新 version ";
}

// `r` 键的入口。选中即应用/清除，不需要额外确认，参照标签系统的交互
// 哲学；应用/清除成功后返回空字符串(静默)，信息栏下一帧自然显示新的
// "风格:"状态——跟 handle_add_tag_result 成功时静默是同一个理由。创建/
// 删除是相对少见、更值得确认的操作，返回非空的状态提示。
enum class RKeyAction { Cancelled, Applied, Cleared, Toggled, Handled };
struct RKeyOutcome {
  RKeyAction action;
  std::string status;
};

RKeyOutcome handle_r_key(pzt::core::ImageId image_id, int banner_row, int start_col,
                         int content_cols) {
  auto presets = presets_for_menu();
  std::string line = " r:清除  v:预览原图  c:新建  d:删除";
  for (std::size_t i = 0; i < presets.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + presets[i].name;
  }
  line += "  Esc 取消";
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));

  char c = read_one_byte();
  if (c == 'r' || c == '0') {
    auto result = pzt::core::set_image_recipe(image_id, std::nullopt);
    if (!result.ok()) return {RKeyAction::Cancelled, " 清除失败,请重试 "};
    return {RKeyAction::Cleared, ""};
  }
  if (c == 'v') {
    return {RKeyAction::Toggled, ""};
  }
  if (c == 'c') {
    return {RKeyAction::Handled, handle_r_create_flow(banner_row, start_col, content_cols)};
  }
  if (c == 'd') {
    auto preset = handle_pick_preset_prompt(banner_row, start_col, content_cols);
    if (!preset) return {RKeyAction::Cancelled, ""};
    return {RKeyAction::Handled,
            handle_pick_version_to_delete_prompt(*preset, banner_row, start_col, content_cols)};
  }
  if (c >= '1' && c <= static_cast<char>('0' + presets.size())) {
    const auto& preset = presets[static_cast<std::size_t>(c - '1')];
    auto recipe_id =
        handle_pick_version_to_apply_prompt(preset, banner_row, start_col, content_cols);
    if (!recipe_id) return {RKeyAction::Cancelled, ""};
    auto result = pzt::core::set_image_recipe(image_id, *recipe_id);
    if (!result.ok()) return {RKeyAction::Cancelled, " 应用失败,请重试 "};
    return {RKeyAction::Applied, ""};
  }
  return {RKeyAction::Cancelled, ""};  // 取消,静默
}

int cmd_new(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt new: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  std::string folder_path =
      args.size() >= 2 ? args[1] : std::filesystem::current_path().string();

  auto result = pzt::core::create_project(name, folder_path);
  if (!result.ok()) {
    switch (result.error()) {
      case pzt::core::CreateProjectError::NameAlreadyExists:
        std::fprintf(stderr, "pzt new: 项目名 '%s' 已存在\n", name.c_str());
        break;
      case pzt::core::CreateProjectError::NoImagesFound:
        std::fprintf(stderr, "pzt new: '%s' 目录下没有找到任何 JPEG 文件\n",
                     folder_path.c_str());
        break;
    }
    return 1;
  }

  // increment 6.4.5:项目一创建就把"废片"系统标签建好——这时候项目刚建
  // 出来,保证没有任何标签,不需要处理"同名标签已经存在但不是系统标签"
  // 这种迁移场景,pzt open 不需要再管这件事。
  pzt::core::ensure_reject_tag(result.value());

  // Look the freshly-created project back up to report its scanned image
  // count - a bit wasteful (re-queries all projects) but this is a one-shot
  // CLI invocation, not a hot path.
  for (const auto& p : pzt::core::list_projects()) {
    if (p.id == result.value()) {
      std::printf("已创建项目 '%s'(%s),共 %lld 张 JPEG\n", p.name.c_str(),
                  p.root_path.c_str(), static_cast<long long>(p.image_count));
      return 0;
    }
  }
  std::printf("已创建项目 '%s'\n", name.c_str());
  return 0;
}

int cmd_list(const std::vector<std::string>& args) {
  (void)args;
  auto projects = pzt::core::list_projects();
  if (projects.empty()) {
    std::printf("(还没有任何项目,用 pzt new 创建一个)\n");
    return 0;
  }
  for (const auto& p : projects) {
    std::printf("%-20s %8lld 张  %s%s\n", p.name.c_str(),
                static_cast<long long>(p.image_count), p.root_path.c_str(),
                p.archived ? "  [已归档]" : "");
  }
  return 0;
}

// increment 6.4.2:三面板固定布局(图片区左上约 80% 宽、信息栏右上、
// banner 底部全宽),备用屏幕缓冲区 + 每帧清除上一帧 placement,修复
// 6.4.1 真机测试时发现的图片重叠残留问题。
int cmd_open(const std::vector<std::string>& args) {
  bool debug_mode = false;
  std::vector<std::string> positional;
  for (const auto& a : args) {
    if (a == "--debug") {
      debug_mode = true;
    } else {
      positional.push_back(a);
    }
  }

  std::optional<pzt::core::ProjectId> id =
      positional.empty()
          ? pzt::core::find_project_by_root_path(std::filesystem::current_path().string())
          : pzt::core::find_project_by_name(positional[0]);
  if (!id) {
    std::fprintf(stderr, "pzt open: 找不到项目,用 pzt list 查看可用项目及其路径\n");
    return 1;
  }

  auto opened = pzt::core::open_project(*id);
  if (!opened.ok()) {
    // id 来自刚成功的查找,理论上不该走到这里,但还是按"不假设它不会发生"
    // 的原则处理,而不是直接解引用。
    std::fprintf(stderr, "pzt open: 找不到项目,用 pzt list 查看可用项目及其路径\n");
    return 1;
  }
  const auto& project = opened.value();

  auto images = pzt::core::list_images(*id);
  if (images.empty()) {
    std::fprintf(stderr, "pzt open: 项目 '%s' 里没有图片\n", project.name.c_str());
    return 1;
  }

  // increment 6.4.5:废片系统标签正常应该在 pzt new 时就建好了,这里不是
  // 为了处理迁移——只是同一个幂等、廉价的 find-or-create,顺带兜住"项目
  // 不是通过更新后的 pzt new 建的"这种边界情况,避免后面用这个 id 时崩溃。
  pzt::core::TagId reject_tag_id = pzt::core::ensure_reject_tag(*id);

  auto mode = pzt::cli::kitty::detect_terminal_mode();
  if (mode.inside_tmux && !mode.passthrough_ok) {
    std::fprintf(stderr,
                 "pzt open: 当前 Tmux 会话未开启 allow-passthrough,Kitty 图形协议无法穿透"
                 "到 Ghostty。请在 tmux.conf 里加 `set -g allow-passthrough on` 后重启会话,"
                 "或在独立 Ghostty 窗口(不经过 Tmux)里直接运行\n");
    return 1;
  }

  const int kDebugRows = 8;
  std::size_t frame = 0;
  const int kImageId = 1;
  const int kBannerRows = 1;
  const char* kBannerText =
      " h/l 上一张/下一张   j/k 下一张/上一张未打标签   space 打标签   x 标记废片"
      "   g 筛选   q 退出 ";
  // j/k 转一整圈都没找到未打标签的图片时,不静默无反应——banner 这一帧显示
  // 这条提示而不是 kBannerText,显示完就清空,下一次不管按什么键都恢复正
  // 常提示。跟 current_id 一样是这个函数作用域内的纯局部状态,不需要额外
  // 的状态机或定时器。
  std::string status_override;

  // increment 6.4.7:退出时打一行 key-to-render 汇总(count/avg/p95/max)
  // ——PRD 验收标准要求"简单的延迟日志"验证浏览大量图片全程无可感知卡
  // 顿,盯着 debug 面板只保留最后 8 行的实时小窗口没法回头核对整个会话,
  // 需要一份事后能看的汇总,不是新的 core 层能力,纯粹是这个函数自己按
  // 键处理耗时的统计,声明在下面这个块外面,这样块结束(AltScreen/
  // CbreakMode 析构、stderr 换回真实终端)之后还能在这打印。退出时打印一
  // 次,不挂在 --debug 后面。
  std::size_t latency_count = 0;
  double latency_sum_ms = 0.0;
  double latency_max_ms = 0.0;
  std::vector<double> latency_samples;

  {
    // 默认把 stderr(core::PrefetchCache 等的延迟日志)整个丢掉,不跟图片画
    // 到同一块屏幕上;--debug 时改成后台收集,画到屏幕底部专门的 debug 区
    // 域。声明在 prefetch 之前、比它晚析构,这样 prefetch 关闭时可能打的最
    // 后几行日志也能被收住。跟 prefetch 一起放进这个块(而不是 cmd_open 的
    // 外层作用域)是 6.4.7 修的一个问题:这两个如果活到 cmd_open 整个函数
    // 返回才析构,块结束之后想打印的退出汇总/退出提示这些"应该在真实终端
    // 上可见"的输出,实际上还是会被 debug_log 占着的重定向吞掉(不管
    // --debug 开没开,DebugLogRedirect 的析构函数才是真正把 stderr 换回真
    // 实终端的地方)——缩小到这个块的作用域,块结束时 debug_log 先析构、
    // stderr 先换回来,后面的打印才真的能看见。
    pzt::cli::term::DebugLogRedirect debug_log(debug_mode, static_cast<std::size_t>(kDebugRows));

    // window 先给个保守默认值——PRD 里"合理默认值待真实素材测出"这个待办不
    // 受这次影响,调优留给以后有真实使用数据再说。
    pzt::core::PrefetchCache prefetch(project.root_path, /*window=*/3, pzt::core::decode_jpeg_file);
    pzt::core::ImageId current_id = images.front().id;
    prefetch.set_current(images, current_id);

    // AltScreen 在 CbreakMode 前构造、后析构:退出时先把输入模式还原、再离
    // 开备用缓冲区,这样即便中途出异常,用户的主屏幕内容也不会被半途切走
    // 又切不回来。
    pzt::cli::term::AltScreen alt_screen;
    pzt::cli::term::CbreakMode cbreak;

    // 上一帧实际渲染的是哪张图——打标签这类不改 current_id 的操作不需要
    // 重新拉取/传输图片本身,只有 current_id 真的变了才需要。
    std::optional<pzt::core::ImageId> last_rendered_id;

    // M1 increment 5:`r v` 临时切换当前图片是否展示风格化效果,纯查看层
    // 面的状态,不碰数据库。导航到新图片时重置为 false(默认展示风格化效
    // 果),只有 style_toggled 为真时才需要在 current_id 没变的情况下也
    // 强制重新走一遍渲染(正常情况下 current_id 不变就不需要重画)。
    bool show_original = false;
    bool style_toggled = false;

    // 上一帧是不是刚显示过 status_override 这种一次性提示——刚显示过的
    // 话,下一次读键不管读到什么都只用来"消除提示",不当成 h/l/j/k/space
    // 的具体动作处理,呼应提示文案里"按任意键继续"这句话:既然说了任意
    // 键,就不应该因为按的不是那几个认识的键就什么反应都没有,也不应该让
    // 这一次按键同时"消除提示"又"顺便导航/打开菜单",那样反而让人搞不清
    // 这次按键到底生效了没有。
    bool showing_status = false;

    // 上一轮是不是 --debug 模式下 poll 超时(没有真实按键)触发的重画——是
    // 的话,这一轮渲染完不打 key-to-render 延迟日志:这条日志的本意是"从
    // 按键到渲染完成"的延迟,超时触发的重画根本没有对应的按键,量出来的
    // 只是这一帧本身的渲染耗时(而且图片这步大概率被跳过,数字会很小),
    // 跟这条日志真正想回答的问题("切图快不快")没关系,混在一起只会让
    // debug 面板看起来像是在不停后台重复干活。
    bool suppress_latency_log = false;

    // increment 6.4.6:当前是否在 g + 数字切出来的筛选视图里,以及筛选到
    // 了哪个标签——跟 current_id 一样是这个函数作用域内的纯局部状态。
    std::optional<pzt::core::TagId> active_filter_tag_id;
    std::string active_filter_tag_name;

    while (true) {
      auto key_time = std::chrono::steady_clock::now();

      std::size_t index = 0;
      const pzt::core::ImageRef* current_ref = nullptr;
      for (std::size_t i = 0; i < images.size(); ++i) {
        if (images[i].id == current_id) {
          index = i;
          current_ref = &images[i];
          break;
        }
      }

      auto term_size = pzt::cli::term::get_terminal_size();
      // 拿不到真实尺寸(非 tty、或者终端没上报像素尺寸)时给一组保守的兜
      // 底值,不让布局计算除零或者算出负数区域。
      int total_cols = term_size.valid ? term_size.cols : 80;
      int total_rows = term_size.valid ? term_size.rows : 24;
      int cell_px_w = term_size.valid ? std::max(1, term_size.pixel_width / term_size.cols) : 8;
      int cell_px_h = term_size.valid ? std::max(1, term_size.pixel_height / term_size.rows) : 16;

      // 整个界面默认只占终端宽度的 70%、居中显示,不铺满整个窗口——以后加
      // 了冒号命令再考虑让这个比例可调。
      const double kWidthRatio = 0.7;
      int ui_cols = std::max(20, static_cast<int>(total_cols * kWidthRatio));
      int start_col = std::max(1, (total_cols - ui_cols) / 2 + 1);

      int content_cols = std::max(1, ui_cols - 2);  // 减去左右各一列边框
      int image_cols = std::max(1, static_cast<int>(content_cols * 0.8));
      int mid_offset = 1 + image_cols;  // 中间竖线相对 start_col 的偏移
      int info_cols = std::max(1, content_cols - image_cols - 2);  // -1: 中间竖线,-1: 留一列空隙
      int info_col = start_col + mid_offset + 2;  // 信息栏内容起始列,跳过竖线和一列空隙

      int border_rows = 2;  // 顶部 + 底部
      int divider_rows = 1 + (debug_mode ? 1 : 0);
      int fixed_rows = border_rows + divider_rows + kBannerRows + (debug_mode ? kDebugRows : 0);
      int top_rows = std::max(1, total_rows - fixed_rows);

      // 画边框:单个外框 + 图片/信息栏之间的竖线分隔,风格照抄设计阶段讨论
      // 过的 ASCII 示意图,不是四个各自独立的小方框。
      {
        int row = 1;
        draw_hline(row++, start_col, ui_cols, "┌", "┐", mid_offset, "┬");
        for (int i = 0; i < top_rows; ++i) draw_vlines(row + i, start_col, ui_cols, mid_offset);
        row += top_rows;
        draw_hline(row++, start_col, ui_cols, "├", "┤", mid_offset, "┴");
        if (debug_mode) {
          for (int i = 0; i < kDebugRows; ++i) draw_vlines(row + i, start_col, ui_cols);
          row += kDebugRows;
          draw_hline(row++, start_col, ui_cols, "├", "┤");
        }
        draw_vlines(row, start_col, ui_cols);
        row++;
        draw_hline(row, start_col, ui_cols, "└", "┘");
      }
      int image_top_row = 2;  // 顶部边框占第 1 行,图片/信息内容从第 2 行开始
      int debug_top_row = 2 + top_rows + 1;  // 图片区 + 分隔线之后
      int banner_row = debug_top_row + (debug_mode ? kDebugRows + 1 : 0);

      // 信息栏:编号、文件名、标签、文件大小,固定在图片区右侧。内容行数
      // 随标签数量变化(标签越多占的行越多)——真机测试发现,标签数变少之
      // 后,上一帧比较靠下的内容(比如"大小:"那一行)不会被这一帧覆盖到,
      // 会一直重影在那。先把整个信息栏区域清空,再画这一帧实际用到的内
      // 容,不管行数怎么变都不会留下上一帧的残留。
      {
        for (int r = 0; r < top_rows; ++r) {
          move_cursor(image_top_row + r, info_col);
          write_stdout(pad_to("", info_cols));
        }
        int row = image_top_row;
        move_cursor(row++, info_col);
        // increment 6.4.6:筛选状态拼在这一行后面,不新增一行——这样下面
        // 每一行(文件名、标签、大小)不管是不是在筛选视图里都是完全一样
        // 的行号计算,切换筛选状态时不会有内容跳动。
        std::string index_line =
            "[" + std::to_string(index + 1) + "/" + std::to_string(images.size()) + "]";
        if (active_filter_tag_id) index_line += "  筛选: " + active_filter_tag_name;
        write_stdout(pad_to(index_line, info_cols));

        move_cursor(row++, info_col);
        write_stdout(pad_to(current_ref ? current_ref->file_name : "?", info_cols));

        row++;  // 空一行
        move_cursor(row++, info_col);
        write_stdout(pad_to("标签:", info_cols));
        auto tags = current_ref ? pzt::core::tags_for_image(current_ref->id)
                                 : std::vector<pzt::core::TagSummary>{};
        if (tags.empty()) {
          move_cursor(row++, info_col);
          write_stdout(pad_to("(无)", info_cols));
        } else {
          for (const auto& t : tags) {
            move_cursor(row++, info_col);
            write_stdout(pad_to(t.name, info_cols));
          }
        }

        row++;  // 空一行
        auto info = current_ref ? pzt::core::get_image(current_ref->id) : std::nullopt;
        if (info) {
          move_cursor(row++, info_col);
          write_stdout(pad_to("大小: " + format_size(info->file_size), info_cols));
        }

        // M1 increment 3:在真正的 `r` 交互(increment 6)和预览渲染
        // (increment 5)落地之前,先在信息栏露出"这张图应用了哪个风格",
        // 方便用 apply-debug 之类的调试命令验证时能直观看到结果,不用每
        // 次都手动查数据库。两层模型(预设/version)用两级缩进画成一棵小
        // 树,不是拼成一行文本——真机测试发现拼一行会在信息栏这种窄列里
        // 被截断,例如"风格: Standard: MyStandard"就被切成了"风格:
        // Standard: MyStanda",看不全。
        row++;  // 空一行
        move_cursor(row++, info_col);
        write_stdout(pad_to("风格:", info_cols));
        auto recipe_id = current_ref ? pzt::core::get_image_recipe(current_ref->id) : std::nullopt;
        auto style = recipe_id ? pzt::core::describe_recipe(*recipe_id) : std::nullopt;
        if (!style) {
          move_cursor(row++, info_col);
          write_stdout(pad_to("  (无)", info_cols));
        } else {
          // M1 increment 5:当前实际渲染的是风格化效果时加粗(`r v` 切到
          // 原图预览时取消),直接呼应"现在看到的是不是风格化效果"这个状
          // 态。粗体转义码要包在 pad_to 算完显示宽度之后的结果外层,不能
          // 传给 pad_to 之前就包——不然转义字节会被 display_width 当成
          // 可见字符,算错截断/补空格的位置。
          bool bold = !show_original;
          auto emit_style_line = [&](const std::string& text) {
            std::string padded = pad_to(text, info_cols);
            write_stdout(bold ? "\x1b[1m" + padded + "\x1b[0m" : padded);
          };
          move_cursor(row++, info_col);
          emit_style_line("  " + style->preset_name);
          if (style->version_name) {
            move_cursor(row++, info_col);
            emit_style_line("    " + *style->version_name);
          }
        }
      }

      // --debug 时,图片/信息栏下方专门留出来的滚动 debug 区——按帧重画最
      // 新的 kDebugRows 行,不是真正的终端滚动区域,但对用户来说效果一样:
      // 新日志进来,老的自然被挤出显示范围。
      if (debug_mode) {
        auto lines = debug_log.snapshot();
        std::size_t begin =
            lines.size() > static_cast<std::size_t>(kDebugRows)
                ? lines.size() - static_cast<std::size_t>(kDebugRows)
                : 0;
        for (int i = 0; i < kDebugRows; ++i) {
          move_cursor(debug_top_row + i, start_col + 1);
          std::size_t idx = begin + static_cast<std::size_t>(i);
          write_stdout(pad_to(idx < lines.size() ? lines[idx] : "", content_cols));
        }
      }

      // Banner:固定在图片/信息栏下方最后一行,边框内全宽。
      move_cursor(banner_row, start_col + 1);
      showing_status = !status_override.empty();
      if (showing_status) {
        // status_override 里的消息大多自带一个尾随空格(跟 kBannerText 的
        // 视觉留白风格一致),直接拼接"  按任意键继续"会在两者之间留出一大
        // 段空白,看起来像隔得很远——先去掉消息自己的尾随空格,用逗号衔接
        // 而不是额外的空格。
        std::string trimmed = status_override;
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        write_stdout(pad_to(trimmed + ",按任意键继续 ", content_cols));
      } else {
        write_stdout(pad_to(kBannerText, content_cols));
      }
      status_override.clear();  // 只显示这一帧,不管接下来按了什么键都恢复正常提示

      // 图片放在信息栏/banner 之后画:真机测试反馈图片显示出来之后,右边
      // 信息栏和底部 banner 的文字有明显的滞后才跟上,怀疑是 Ghostty 处理
      // Kitty 图片协议命令(读临时文件、解码、合成)这一步在它自己的主循环
      // 里是同步/阻塞的,会顺带卡住紧跟在图片命令后面的文字——即便我们这
      // 边是几乎同时把所有这些控制序列写出去的。这几行文字本身很小、写
      // 出去的成本可以忽略,调整顺序让文字先于图片写出去,这样即便终端处
      // 理图片这一步确实慢,文字至少能立刻显示,不用跟着一起卡住。打标签
      // 这类操作不会改 current_id,不需要重新清除/传输同一张图——真机测试
      // 发现,不加这个判断的话,打个标签也会因为整帧重画而卡顿一下,尽管
      // 图片内容根本没变。只有 current_id 真的变了才重新走一遍"清掉上一
      // 帧的图 -> 取解码结果 -> 缩放 -> 传输"这一整套。
      bool navigated = (last_rendered_id != current_id);
      if (navigated) {
        show_original = false;  // 每次导航到新图片,默认展示风格化效果
      }
      if (navigated || style_toggled) {
        // 每帧先清掉上一帧的图,再画新的——这是修复 6.4.1 重叠残留问题的
        // 关键一步,没有它,旧 placement 不会自动消失。
        pzt::cli::kitty::clear_placement(STDOUT_FILENO, mode, kImageId);

        auto decoded = prefetch.get(current_id);
        if (decoded.ok()) {
          const auto& img = decoded.value();
          auto fit = pzt::cli::kitty::fit_within(img.width, img.height, image_cols * cell_px_w,
                                                  top_rows * cell_px_h);
          int target_cols = std::max(1, fit.width / cell_px_w);
          int target_rows = std::max(1, fit.height / cell_px_h);

          // 真机测试确认过:每帧把原始分辨率的 RGBA(可能几 MB 到近十 MB)
          // 整个丢给终端,终端自己读临时文件+解码+缩放显示,是切图卡顿的
          // 实际来源——即便我们这边 prefetch 已经命中、解码耗时为 0。先
          // 在这边缩小到面板大致能显示的尺寸,大幅减少终端侧要处理的数
          // 据量。
          auto resized = pzt::core::resize_rgba(img, fit.width, fit.height);
          const auto& downsampled = resized.ok() ? resized.value() : img;

          // M1 increment 5:在降采样之后、发给终端之前应用 recipe。
          // thread_count=1 同步执行——Phase 0 spike 已经验证过预览分辨率
          // 下这一步足够便宜(10-22ms),不需要额外的后台线程或缓存;这个
          // if 块本来就只在导航或 `r v` 切换时才跑,不会每帧都重算。
          // show_original 为真时(用户按了 r v 切到原图预览)跳过渲染。
          std::optional<pzt::core::DecodedImage> styled;
          auto recipe_id = pzt::core::get_image_recipe(current_id);
          if (recipe_id && !show_original) {
            auto render_result = pzt::core::render(downsampled, *recipe_id, 1);
            if (render_result.ok()) styled = std::move(render_result.value());
            // render 失败(比如引用了一个数据损坏的 recipe_id)时静默回退
            // 到未处理的画面,不阻断浏览,跟"图片解码失败,跳过"是同一种
            // 防御精神。
          }
          const auto& to_render = styled ? *styled : downsampled;

          move_cursor(image_top_row, start_col + 1);
          std::string tmp_path = pzt::cli::kitty::make_tmp_path(
              std::to_string(getpid()) + "_" + std::to_string(frame++));
          auto rendered = pzt::cli::kitty::render_rgba_via_tmpfile(
              STDOUT_FILENO, mode, to_render, kImageId, tmp_path, target_cols, target_rows);
          if (!rendered.ok()) {
            std::fprintf(stderr, "pzt open: 渲染失败\n");
          }
        } else {
          std::fprintf(stderr, "pzt open: 图片解码失败,跳过\n");
        }
        last_rendered_id = current_id;
        style_toggled = false;
      }

      if (!suppress_latency_log) {
        double key_to_render_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - key_time)
                .count();
        std::fprintf(stderr, "[pzt open] key-to-render %.2fms\n", key_to_render_ms);
        ++latency_count;
        latency_sum_ms += key_to_render_ms;
        latency_max_ms = std::max(latency_max_ms, key_to_render_ms);
        latency_samples.push_back(key_to_render_ms);
      }

      char c = 0;
      if (showing_status) {
        // 刚显示过一次性提示("按任意键继续"),这一次读键不管读到什么字
        // 节都只用来消除提示、跳回外层循环重画一次正常画面,不当成
        // h/l/j/k/space 的具体动作执行——否则这一次按键会同时"消除提示"
        // 又"顺便导航/打开菜单",容易让人搞不清这次按键到底生效了没有。
        ssize_t n = read(STDIN_FILENO, &c, 1);
        showing_status = false;
        suppress_latency_log = false;  // 这一轮读到了真实按键(用来消除提示)
        if (n <= 0) break;  // 真正的 EOF/出错,当退出处理
        continue;
      }

      // 不支持的键直接在这个内层循环里吃掉,继续读下一个字节——不 continue
      // 回外层 while,那样会导致整个画面(边框、图片、信息栏、banner)重新
      // 渲染一遍,一次误按不支持的键就能看到明显的闪烁。--debug 模式下例
      // 外:每一轮先 poll 一次,超时(没有任何按键)就直接 continue 回外层
      // 重画,刷新 debug 面板;不开 --debug 时维持原来单纯阻塞 read 的写
      // 法,不引入 poll 的额外开销。
      bool timed_out = false;
      while (true) {
        if (debug_mode) {
          if (!stdin_ready(300)) {
            timed_out = true;
            break;
          }
        }
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
          c = 'q';
          break;
        }
        if (c == 'q' || c == 'h' || c == 'l' || c == 'j' || c == 'k' || c == ' ' || c == 'x' ||
            c == 'g' || c == 'r') {
          break;
        }
      }
      if (timed_out) {
        suppress_latency_log = true;  // 没有按键,只是刷新 debug 面板,不处理导航
        continue;
      }
      suppress_latency_log = false;  // 这一轮确实读到了真实按键
      if (c == 'q') break;

      if (c == 'h') {
        current_id = pzt::core::prev_image(images, current_id).value_or(current_id);
      } else if (c == 'l') {
        current_id = pzt::core::next_image(images, current_id).value_or(current_id);
      } else if (c == 'j') {
        // 筛选视图里每张图按定义都至少有筛选到的那个标签,"下一个未打标
        // 签的" 在这个语境下没有意义,永远立刻报"全部打完"——不是 bug,但
        // 体验上很尴尬,筛选生效时退化成跟 l 一样的普通下一张。
        if (active_filter_tag_id) {
          current_id = pzt::core::next_image(images, current_id).value_or(current_id);
        } else {
          auto next = pzt::core::next_untagged(images, current_id);
          if (next) {
            current_id = *next;
          } else {
            status_override = " 所有图片都已打过标签 ";
          }
        }
      } else if (c == 'k') {
        if (active_filter_tag_id) {
          current_id = pzt::core::prev_image(images, current_id).value_or(current_id);
        } else {
          auto prev = pzt::core::prev_untagged(images, current_id);
          if (prev) {
            current_id = *prev;
          } else {
            status_override = " 所有图片都已打过标签 ";
          }
        }
      } else if (c == ' ') {
        if (current_ref) {
          status_override = handle_space_key(*id, reject_tag_id, current_ref->id, banner_row,
                                              start_col, content_cols);
        }
        // current_id 不变,跟其它分支一样落到下面的 set_current + 循环顶部
        // 整屏重绘,信息栏会自然显示打标签之后的结果。
      } else if (c == 'x') {
        // 标记为废片的直达快捷键,等价于 space + 0/space - 0,但不用先开
        // 菜单——废片预期是使用频率最高的标签,值得单独开一个键。做成开
        // 关切换(已经标了就摘掉):误按一下能直接再按一次撤销,不需要先
        // 开 space 菜单走摘除流程。
        if (current_ref) {
          auto current_tags = pzt::core::tags_for_image(current_ref->id);
          bool already_tagged = std::any_of(
              current_tags.begin(), current_tags.end(),
              [&](const auto& t) { return t.id == reject_tag_id; });
          if (already_tagged) {
            auto result = pzt::core::remove_tag(current_ref->id, reject_tag_id);
            status_override = result.ok() ? "" : " 摘标签失败,请重试 ";
          } else {
            status_override = handle_add_tag_result(reject_tag_id, current_ref->id, banner_row,
                                                     start_col, content_cols);
          }
        }
      } else if (c == 'g') {
        // g + 数字切换到只浏览该标签下图片的筛选视图,g + g 清除筛选回到
        // 完整项目——数字编号复用跟 space 菜单同一套 tags_for_menu。
        auto tags = tags_for_menu(*id);
        auto decision = handle_g_key_prompt(reject_tag_id, tags, active_filter_tag_id,
                                             active_filter_tag_name, banner_row, start_col,
                                             content_cols);

        if (decision.action == GKeyAction::Handled) {
          status_override = decision.status;
        } else if (decision.action == GKeyAction::ApplyFilter) {
          // 真机测试反馈 g + 数字筛选有明显卡顿,查出来是 image_tags 按
          // tag_id 过滤没有索引可用(见 core/db/schema.cpp 的说明,已经
          // 补上索引)——这里打一下查询本身的耗时,debug 面板能直接看到
          // 这一步占了多少,跟后面"切到新图片要重新解码"那部分区分开。
          auto filter_t0 = std::chrono::steady_clock::now();
          auto filtered = pzt::core::filter_by_tag(decision.tag_id);
          double filter_query_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - filter_t0)
                                        .count();
          std::fprintf(stderr, "[pzt open] filter_by_tag tag_id=%lld %.2fms\n",
                       static_cast<long long>(decision.tag_id), filter_query_ms);
          if (!filtered.ok()) {
            status_override = " 筛选失败,请重试 ";  // 结构上不可能,防御性处理
          } else if (filtered.value().empty()) {
            status_override = " 该标签下暂无图片 ";  // 拒绝切换,images/current_id 不变
          } else {
            // 注意顺序:先用 filtered.value() 算出 new_current,再 move,
            // 不然 move 之后 filtered.value() 已经是空壳。
            pzt::core::ImageId new_current =
                resolve_current_after_switch(filtered.value(), current_id);
            images = std::move(filtered.value());
            current_id = new_current;
            active_filter_tag_id = decision.tag_id;
            active_filter_tag_name = decision.tag_name;
          }
        } else if (decision.action == GKeyAction::ClearFilter) {
          if (active_filter_tag_id) {
            auto full = pzt::core::list_images(*id);
            pzt::core::ImageId new_current = resolve_current_after_switch(full, current_id);
            images = std::move(full);
            current_id = new_current;
            active_filter_tag_id.reset();
            active_filter_tag_name.clear();
          }
          // 不在筛选中时 g+g 是空操作:不查库、不提示,静默——避免每次误
          // 按 g+g 在未筛选状态下也触发一次不必要的 list_images 查询。
        }
        // Cancel:什么都不做,静默
      } else if (c == 'r') {
        // increment 6:完整的 `r` 前缀键交互,见 handle_r_key。应用/清除
        // 需要重新走一遍渲染(recipe_id 变了或者切到原图预览),交给
        // style_toggled 触发;创建/删除不影响当前图片的 recipe_id,不需
        // 要强制重画。
        if (current_ref) {
          auto outcome = handle_r_key(current_ref->id, banner_row, start_col, content_cols);
          status_override = outcome.status;
          if (outcome.action == RKeyAction::Applied || outcome.action == RKeyAction::Cleared) {
            show_original = false;
            style_toggled = true;
          } else if (outcome.action == RKeyAction::Toggled) {
            show_original = !show_original;
            style_toggled = true;
          }
        }
      }
      prefetch.set_current(images, current_id);
    }

    // 退出前显式删掉最后一帧的 placement——AltScreen 切回主屏幕缓冲区、
    // 甚至用户手动跑 `clear`,都清不掉 Kitty 协议画出来的图片,那是叠加在
    // 文字网格之上的独立层,只有协议自己的 delete 命令能清。
    pzt::cli::kitty::clear_placement(STDOUT_FILENO, mode, kImageId);
  }  // AltScreen/CbreakMode 析构,自动还原终端设置

  if (latency_count > 0) {
    std::sort(latency_samples.begin(), latency_samples.end());
    std::size_t p95_index = std::min(latency_samples.size() - 1,
                                      static_cast<std::size_t>(latency_samples.size() * 0.95));
    std::fprintf(stderr, "[pzt open] key-to-render summary: n=%zu avg=%.2fms p95=%.2fms max=%.2fms\n",
                 latency_count, latency_sum_ms / static_cast<double>(latency_count),
                 latency_samples[p95_index], latency_max_ms);
  }

  std::fprintf(stderr, "已退出浏览\n");
  return 0;
}

int cmd_archive(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt archive: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  auto id = pzt::core::find_project_by_name(name);
  if (!id) {
    std::fprintf(stderr, "pzt archive: 找不到项目 '%s',用 pzt list 查看可用项目\n", name.c_str());
    return 1;
  }
  if (!pzt::core::archive_project(*id).ok()) {
    std::fprintf(stderr, "pzt archive: 找不到项目 '%s'\n", name.c_str());
    return 1;
  }
  std::printf("已归档项目 '%s'\n", name.c_str());
  return 0;
}

int cmd_delete(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt delete: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  auto id = pzt::core::find_project_by_name(name);
  if (!id) {
    std::fprintf(stderr, "pzt delete: 找不到项目 '%s',用 pzt list 查看可用项目\n", name.c_str());
    return 1;
  }

  std::printf(
      "即将删除项目 '%s' 的全部标签与浏览状态,不影响磁盘上的照片文件,此操作不可撤销。\n",
      name.c_str());
  std::printf("请再次输入项目名确认删除: ");
  std::fflush(stdout);
  std::string confirmation;
  if (!std::getline(std::cin, confirmation) || confirmation != name) {
    std::printf("已取消,项目未被删除\n");
    return 1;
  }

  if (!pzt::core::delete_project(*id).ok()) {
    std::fprintf(stderr, "pzt delete: 找不到项目 '%s'\n", name.c_str());
    return 1;
  }
  std::printf("已删除项目 '%s' 的元数据\n", name.c_str());
  return 0;
}

int tag_list(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt tag list: 缺少 <project_name>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag list", args[0]);
  if (!project_id) return 1;

  auto tags = pzt::core::list_tags(*project_id);
  if (tags.empty()) {
    std::printf("(还没有任何标签,用 pzt tag create 创建一个)\n");
    return 0;
  }
  for (const auto& t : tags) {
    std::printf("%-16s %6lld 张%s%s%s\n", t.name.c_str(),
                static_cast<long long>(t.tagged_count),
                t.cap ? ("  cap=" + std::to_string(*t.cap)).c_str() : "",
                t.is_ordered ? "  ordered" : "", t.is_system ? "  system" : "");
  }
  return 0;
}

int cmd_rescan(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt rescan: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  bool prune = true;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--no-prune") {
      prune = false;
    } else {
      std::fprintf(stderr, "pzt rescan: 未知参数 '%s'\n", args[i].c_str());
      print_usage();
      return 1;
    }
  }

  auto project_id = resolve_project("pzt rescan", name);
  if (!project_id) return 1;

  auto result = pzt::core::rescan_project(*project_id, prune);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt rescan: 找不到项目 '%s'\n", name.c_str());
    return 1;
  }
  std::printf("新增 %lld 张,清除 %lld 张磁盘上已消失的记录,项目现在共 %lld 张\n",
              static_cast<long long>(result.value().added_count),
              static_cast<long long>(result.value().removed_count),
              static_cast<long long>(result.value().total_count));
  return 0;
}

int cmd_export(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    std::fprintf(stderr,
                 "pzt export: 缺少 <project_name> <tag_name> <output_folder>\n");
    print_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt export", args[0]);
  if (!project_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[1]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt export: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }
  std::string output_folder = expand_home_path(args[2]);

  auto link_mode = pzt::core::LinkMode::Copy;
  for (std::size_t i = 3; i < args.size(); ++i) {
    if (args[i] == "--link") link_mode = pzt::core::LinkMode::Symlink;
  }

  auto result = pzt::core::export_tag(*tag_id, output_folder, link_mode);
  if (!result.ok()) {
    if (result.error() == pzt::core::ExportTagError::IoError) {
      std::fprintf(stderr, "pzt export: 导出目标 '%s' 无法写入(权限不足或路径被占用)\n",
                   output_folder.c_str());
    } else {
      std::fprintf(stderr, "pzt export: 找不到标签 '%s'\n", args[1].c_str());
    }
    return 1;
  }

  const auto& r = result.value();
  if (r.exported_count == 0 && r.skipped.empty()) {
    std::printf("标签 '%s' 下没有图片,未导出\n", args[1].c_str());
    return 0;
  }
  std::printf("已导出 %d 张到 '%s'", r.exported_count, output_folder.c_str());
  if (r.created_output_folder) std::printf("(目录不存在,已新建)");
  if (r.skipped.empty()) {
    std::printf("\n");
  } else {
    std::printf(",跳过 %zu 张:\n", r.skipped.size());
    for (const auto& s : r.skipped) {
      std::printf("  - %s: %s\n", s.file_name.c_str(), s.reason.c_str());
    }
  }
  return 0;
}

int cmd_tag(const std::vector<std::string>& args) {
  if (args.empty()) {
    print_tag_usage();
    return 1;
  }
  const std::string& verb = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  if (verb == "list") return tag_list(rest);

  std::fprintf(stderr, "pzt tag: 未知子命令 '%s'\n", verb.c_str());
  print_tag_usage();
  return 1;
}

// increment 2:`pzt recipe rename`/`delete` 用 "<preset_name>:<version_
// number>" 这种地址寻址一个 version——预设用名字(固定、稳定),version 用
// 该预设下的编号(排除已软删除的,按 id 升序排位,即 recipe_list 打印出来
// 的那个编号)。这纯粹是 CLI 输入约定,不是业务概念,不下沉进 core。
std::optional<std::pair<std::string, int>> parse_recipe_address(const std::string& address) {
  auto colon = address.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= address.size()) return std::nullopt;
  std::string preset_name = address.substr(0, colon);
  std::string number_part = address.substr(colon + 1);
  try {
    std::size_t consumed = 0;
    long long n = std::stoll(number_part, &consumed);
    if (consumed != number_part.size() || n <= 0) return std::nullopt;
    return std::make_pair(preset_name, static_cast<int>(n));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// 第三处需要"按名字找预设"的地方(create-debug、resolve_recipe_address
// 自己各写过一遍)，这次抽成共用的小函数。
std::optional<pzt::core::RecipeId> find_preset_by_name(const std::string& name) {
  auto presets = pzt::core::list_presets();
  auto it = std::find_if(presets.begin(), presets.end(),
                          [&](const auto& p) { return p.name == name; });
  return it == presets.end() ? std::nullopt : std::optional(it->id);
}

std::optional<pzt::core::RecipeId> resolve_recipe_address(const std::string& preset_name,
                                                            int version_number) {
  auto preset_id = find_preset_by_name(preset_name);
  if (!preset_id) return std::nullopt;

  auto versions = pzt::core::list_versions(*preset_id);
  int v = 1;
  for (const auto& ver : versions) {
    if (ver.deleted) continue;
    if (v == version_number) return ver.id;
    ++v;
  }
  return std::nullopt;
}

// 预设是全局的,不属于任何项目,不需要 <project_name> 参数,跟 tag_list
// 的写法不一样。increment 2:版本编号只发给未软删除的(按 id 升序排位,
// 跟 `r` 菜单看到的编号一致,也是 pzt recipe rename/delete 寻址语法里
// <version_number> 的定义);已删除的不给编号(不再是能被寻址的目标),
// 单独标"[已删除]",直接复用 M0 pzt list 展示归档项目的既有模式。
int recipe_list(const std::vector<std::string>& args) {
  if (!args.empty()) {
    std::fprintf(stderr, "pzt recipe list: 不接受参数\n");
    print_recipe_usage();
    return 1;
  }
  auto presets = pzt::core::list_presets();
  if (presets.empty()) {
    std::printf("(没有任何预设)\n");
    return 0;
  }
  int i = 1;
  for (const auto& p : presets) {
    std::printf("%-3d %s\n", i++, p.name.c_str());
    auto versions = pzt::core::list_versions(p.id);
    int v = 1;
    for (const auto& ver : versions) {
      std::string name = ver.name.value_or("(未命名)");
      if (ver.deleted) {
        std::printf("      -   %-14s [已删除]\n", name.c_str());
      } else {
        std::printf("      %-3d %-14s highlights=%.1f shadows=%.1f wb_r=%.1f wb_b=%.1f\n", v++,
                     name.c_str(), ver.highlights, ver.shadows, ver.wb_shift_r, ver.wb_shift_b);
      }
    }
  }
  return 0;
}

int recipe_rename(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "pzt recipe rename: 缺少 <preset>:<version_number> <new_name>\n");
    print_recipe_usage();
    return 1;
  }
  auto address = parse_recipe_address(args[0]);
  if (!address) {
    std::fprintf(stderr, "pzt recipe rename: 无法解析 '%s',格式应为 <preset>:<version_number>\n",
                 args[0].c_str());
    return 1;
  }
  auto id = resolve_recipe_address(address->first, address->second);
  if (!id) {
    std::fprintf(stderr, "pzt recipe rename: 找不到 '%s'\n", args[0].c_str());
    return 1;
  }
  if (!pzt::core::rename_version(*id, args[1]).ok()) {
    std::fprintf(stderr, "pzt recipe rename: 操作失败\n");
    return 1;
  }
  std::printf("已重命名为 '%s'\n", args[1].c_str());
  return 0;
}

int recipe_delete(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt recipe delete: 缺少 <preset>:<version_number>\n");
    print_recipe_usage();
    return 1;
  }
  auto address = parse_recipe_address(args[0]);
  if (!address) {
    std::fprintf(stderr, "pzt recipe delete: 无法解析 '%s',格式应为 <preset>:<version_number>\n",
                 args[0].c_str());
    return 1;
  }
  auto id = resolve_recipe_address(address->first, address->second);
  if (!id) {
    std::fprintf(stderr, "pzt recipe delete: 找不到 '%s'\n", args[0].c_str());
    return 1;
  }
  if (!pzt::core::delete_version(*id).ok()) {
    std::fprintf(stderr, "pzt recipe delete: 操作失败\n");
    return 1;
  }
  std::printf("已删除 '%s'(软删除,已经应用它的图片渲染不受影响)\n", args[0].c_str());
  return 0;
}

int cmd_recipe(const std::vector<std::string>& args) {
  if (args.empty()) {
    print_recipe_usage();
    return 1;
  }
  const std::string& verb = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  if (verb == "list") return recipe_list(rest);
  if (verb == "rename") return recipe_rename(rest);
  if (verb == "delete") return recipe_delete(rest);

  std::fprintf(stderr, "pzt recipe: 未知子命令 '%s'\n", verb.c_str());
  print_recipe_usage();
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }

  std::string subcommand = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);

  if (subcommand == "new") return cmd_new(args);
  if (subcommand == "list") return cmd_list(args);
  if (subcommand == "open") return cmd_open(args);
  if (subcommand == "archive") return cmd_archive(args);
  if (subcommand == "delete") return cmd_delete(args);
  if (subcommand == "rescan") return cmd_rescan(args);
  if (subcommand == "export") return cmd_export(args);
  if (subcommand == "tag") return cmd_tag(args);
  if (subcommand == "recipe") return cmd_recipe(args);

  std::fprintf(stderr, "pzt: 未知子命令 '%s'\n", subcommand.c_str());
  print_usage();
  return 1;
}
