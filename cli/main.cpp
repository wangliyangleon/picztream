#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

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
               "  pzt open [project_name] [--debug]  (h/l 上一张/下一张,q 退出;"
               "increment 6.4 后续步骤会加 j/k/space/x/g+数字;--debug 时在图片下方"
               "开一块区域滚动显示内部日志,默认不显示也不产生这些日志)\n"
               "  pzt archive <project_name>\n"
               "  pzt delete <project_name>\n"
               "  pzt rescan <project_name> [--no-prune]  (默认会清除磁盘上已消失的"
               "文件记录,连带清掉其标签;对着可能暂时没挂载完整的存储位置跑时,"
               "加 --no-prune 跳过清理)\n"
               "  pzt export <project_name> <tag_name> <output_folder> [--link]\n"
               "  pzt decode <jpeg_path>  (临时调试命令,验证 core/decode 的解码管线通,"
               "increment 6 后续步骤会接入真正的浏览渲染循环)\n"
               "  pzt render <jpeg_path>  (临时调试命令,验证 cli/kitty 渲染管线通,"
               "需要在真实 Ghostty 终端里跑才能看到画面)\n"
               "  pzt prefetch <project_name> [window]  (临时调试命令,验证 core/browse 的"
               "预取环形缓冲区随导航移动、后台解码线程确实把像素准备好,"
               "increment 6.4 会把它接进真正的全键盘循环)\n"
               "  pzt tag create|list|add|remove|replace ...  "
               "(临时调试命令,increment 6 会被全键盘交互替换,pzt tag 查看用法)\n"
               "  pzt browse next|prev|next-untagged|prev-untagged|filter ...  "
               "(同上,pzt browse 查看用法)\n");
}

void print_browse_usage() {
  std::fprintf(stderr,
               "usage (临时调试命令,后续会被全键盘交互替换):\n"
               "  pzt browse next <project_name> [current_image_relative_path]\n"
               "  pzt browse prev <project_name> [current_image_relative_path]\n"
               "  pzt browse next-untagged <project_name> [current_image_relative_path]\n"
               "  pzt browse prev-untagged <project_name> [current_image_relative_path]\n"
               "  pzt browse filter <project_name> <tag_name>\n");
}

void print_tag_usage() {
  std::fprintf(stderr,
               "usage (临时调试命令,后续会被全键盘交互替换):\n"
               "  pzt tag create <project_name> <tag_name> [--cap N] [--ordered]\n"
               "  pzt tag list <project_name>\n"
               "  pzt tag add <project_name> <image_relative_path> <tag_name>\n"
               "  pzt tag remove <project_name> <image_relative_path> <tag_name>\n"
               "  pzt tag replace <project_name> <tag_name> <old_image_relative_path> "
               "<new_image_relative_path>\n");
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

std::optional<pzt::core::ImageId> resolve_image(const std::string& cmd,
                                                 pzt::core::ProjectId project_id,
                                                 const std::string& relative_path) {
  auto id = pzt::core::find_image_by_path(project_id, relative_path);
  if (!id) {
    std::fprintf(stderr, "%s: 项目内找不到文件 '%s'\n", cmd.c_str(), relative_path.c_str());
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

std::string truncate_text(const std::string& s, std::size_t max_len) {
  if (s.size() <= max_len) return s;
  if (max_len <= 3) return s.substr(0, max_len);
  return s.substr(0, max_len - 3) + "...";
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

// 截断/补空格到固定的显示宽度——按边框内容区写字的地方都要用这个,而不是
// 裸写字符串,否则这一帧内容比上一帧短的时候,会在文字和右边框之间留下
// 上一帧的残留字符。跟 truncate_text 一样按字节数近似显示宽度,中文字符
// 会被算窄了(UTF-8 里占 3 字节但终端里占 2 列),这是继承自 6.4.2 就有的
// 简化,这次没有额外解决。
std::string pad_to(const std::string& s, std::size_t width) {
  std::string t = truncate_text(s, width);
  if (t.size() < width) t += std::string(width - t.size(), ' ');
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

  auto mode = pzt::cli::kitty::detect_terminal_mode();
  if (mode.inside_tmux && !mode.passthrough_ok) {
    std::fprintf(stderr,
                 "pzt open: 当前 Tmux 会话未开启 allow-passthrough,Kitty 图形协议无法穿透"
                 "到 Ghostty。请在 tmux.conf 里加 `set -g allow-passthrough on` 后重启会话,"
                 "或在独立 Ghostty 窗口(不经过 Tmux)里直接运行\n");
    return 1;
  }

  // 默认把 stderr(core::PrefetchCache 等的延迟日志)整个丢掉,不跟图片画
  // 到同一块屏幕上;--debug 时改成后台收集,画到屏幕底部专门的 debug 区
  // 域。声明在 prefetch 之前、比它晚析构,这样 prefetch 关闭时可能打的最
  // 后几行日志也能被收住。
  const int kDebugRows = 8;
  pzt::cli::term::DebugLogRedirect debug_log(debug_mode, static_cast<std::size_t>(kDebugRows));

  // window 先给个保守默认值——PRD 里"合理默认值待真实素材测出"这个待办不
  // 受这次影响,调优留给以后有真实使用数据再说。
  pzt::core::PrefetchCache prefetch(project.root_path, /*window=*/3, pzt::core::decode_jpeg_file);
  pzt::core::ImageId current_id = images.front().id;
  prefetch.set_current(images, current_id);

  std::size_t frame = 0;
  const int kImageId = 1;
  const int kBannerRows = 1;
  const char* kBannerText = " h/l 上一张/下一张   q 退出 ";

  {
    // AltScreen 在 CbreakMode 前构造、后析构:退出时先把输入模式还原、再离
    // 开备用缓冲区,这样即便中途出异常,用户的主屏幕内容也不会被半途切走
    // 又切不回来。
    pzt::cli::term::AltScreen alt_screen;
    pzt::cli::term::CbreakMode cbreak;

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

      // 每帧先清掉上一帧的图,再画新的——这是修复 6.4.1 重叠残留问题的关键
      // 一步,没有它,旧 placement 不会自动消失。
      pzt::cli::kitty::clear_placement(STDOUT_FILENO, mode, kImageId);

      auto decoded = prefetch.get(current_id);
      if (decoded.ok()) {
        const auto& img = decoded.value();
        auto fit = pzt::cli::kitty::fit_within(img.width, img.height, image_cols * cell_px_w,
                                                top_rows * cell_px_h);
        int target_cols = std::max(1, fit.width / cell_px_w);
        int target_rows = std::max(1, fit.height / cell_px_h);

        // 真机测试确认过:每帧把原始分辨率的 RGBA(可能几 MB 到近十 MB)整
        // 个丢给终端,终端自己读临时文件+解码+缩放显示,是切图卡顿的实际
        // 来源——即便我们这边 prefetch 已经命中、解码耗时为 0。先在这边缩
        // 小到面板大致能显示的尺寸,大幅减少终端侧要处理的数据量。
        auto resized = pzt::core::resize_rgba(img, fit.width, fit.height);
        const auto& to_render = resized.ok() ? resized.value() : img;

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

      // 信息栏:编号、文件名、标签、文件大小,固定在图片区右侧。
      {
        int row = image_top_row;
        move_cursor(row++, info_col);
        write_stdout(pad_to("[" + std::to_string(index + 1) + "/" + std::to_string(images.size()) +
                                 "]",
                             info_cols));

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
      write_stdout(pad_to(kBannerText, content_cols));

      double key_to_render_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - key_time)
              .count();
      std::fprintf(stderr, "[pzt open] key-to-render %.2fms\n", key_to_render_ms);

      // 不支持的键直接在这个内层循环里吃掉,继续读下一个字节——不 continue
      // 回外层 while,那样会导致整个画面(边框、图片、信息栏、banner)重新
      // 渲染一遍,一次误按不支持的键就能看到明显的闪烁。
      char c = 0;
      while (true) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
          c = 'q';
          break;
        }
        if (c == 'q' || c == 'h' || c == 'l') break;
      }
      if (c == 'q') break;

      if (c == 'h') {
        current_id = pzt::core::prev_image(images, current_id).value_or(current_id);
      } else {
        current_id = pzt::core::next_image(images, current_id).value_or(current_id);
      }
      prefetch.set_current(images, current_id);
    }

    // 退出前显式删掉最后一帧的 placement——AltScreen 切回主屏幕缓冲区、
    // 甚至用户手动跑 `clear`,都清不掉 Kitty 协议画出来的图片,那是叠加在
    // 文字网格之上的独立层,只有协议自己的 delete 命令能清。
    pzt::cli::kitty::clear_placement(STDOUT_FILENO, mode, kImageId);
  }  // AltScreen/CbreakMode 析构,自动还原终端设置

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

int tag_create(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "pzt tag create: 缺少 <project_name> <tag_name>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag create", args[0]);
  if (!project_id) return 1;
  const std::string& tag_name = args[1];

  std::optional<std::int64_t> cap;
  bool is_ordered = false;
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--cap" && i + 1 < args.size()) {
      cap = std::atoll(args[++i].c_str());
    } else if (args[i] == "--ordered") {
      is_ordered = true;
    }
  }

  auto result = pzt::core::create_tag(*project_id, tag_name, cap, is_ordered);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt tag create: 标签 '%s' 已存在\n", tag_name.c_str());
    return 1;
  }
  std::printf("已创建标签 '%s'%s%s\n", tag_name.c_str(),
              cap ? (" cap=" + std::to_string(*cap)).c_str() : "",
              is_ordered ? " ordered" : "");
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

void print_cap_exceeded(const pzt::core::CapExceededInfo& info) {
  std::fprintf(stderr, "标签已满(上限 %lld 张),现有条目:\n", static_cast<long long>(info.cap));
  int i = 1;
  for (const auto& entry : info.existing_entries) {
    std::fprintf(stderr, "  %d. %s\n", i++, entry.file_name.c_str());
  }
  std::fprintf(stderr, "用 pzt tag replace 指定要替换的文件\n");
}

int tag_add(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    std::fprintf(stderr, "pzt tag add: 缺少 <project_name> <image_relative_path> <tag_name>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag add", args[0]);
  if (!project_id) return 1;
  auto image_id = resolve_image("pzt tag add", *project_id, args[1]);
  if (!image_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[2]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt tag add: 找不到标签 '%s'\n", args[2].c_str());
    return 1;
  }

  auto result = pzt::core::add_tag(*image_id, *tag_id);
  if (!result.ok()) {
    const auto& err = result.error();
    switch (err.kind) {
      case pzt::core::AddTagFailureKind::TagNotFound:
        std::fprintf(stderr, "pzt tag add: 找不到标签\n");
        break;
      case pzt::core::AddTagFailureKind::ImageNotFound:
        std::fprintf(stderr, "pzt tag add: 找不到图片\n");
        break;
      case pzt::core::AddTagFailureKind::ProjectMismatch:
        std::fprintf(stderr, "pzt tag add: 图片和标签不属于同一个项目\n");
        break;
      case pzt::core::AddTagFailureKind::CapExceeded:
        print_cap_exceeded(*err.cap_info);
        break;
    }
    return 1;
  }
  std::printf("已给 '%s' 打上标签 '%s'\n", args[1].c_str(), args[2].c_str());
  return 0;
}

int tag_remove(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    std::fprintf(stderr, "pzt tag remove: 缺少 <project_name> <image_relative_path> <tag_name>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag remove", args[0]);
  if (!project_id) return 1;
  auto image_id = resolve_image("pzt tag remove", *project_id, args[1]);
  if (!image_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[2]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt tag remove: 找不到标签 '%s'\n", args[2].c_str());
    return 1;
  }

  if (!pzt::core::remove_tag(*image_id, *tag_id).ok()) {
    std::fprintf(stderr, "pzt tag remove: 找不到标签或图片\n");
    return 1;
  }
  std::printf("已移除 '%s' 的标签 '%s'\n", args[1].c_str(), args[2].c_str());
  return 0;
}

int tag_replace(const std::vector<std::string>& args) {
  if (args.size() < 4) {
    std::fprintf(stderr,
                 "pzt tag replace: 缺少 <project_name> <tag_name> "
                 "<old_image_relative_path> <new_image_relative_path>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag replace", args[0]);
  if (!project_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[1]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt tag replace: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }
  auto old_image_id = resolve_image("pzt tag replace", *project_id, args[2]);
  if (!old_image_id) return 1;
  auto new_image_id = resolve_image("pzt tag replace", *project_id, args[3]);
  if (!new_image_id) return 1;

  auto result = pzt::core::replace_tag_entry(*tag_id, *old_image_id, *new_image_id);
  if (!result.ok()) {
    switch (result.error()) {
      case pzt::core::ReplaceTagError::TagNotFound:
        std::fprintf(stderr, "pzt tag replace: 找不到标签\n");
        break;
      case pzt::core::ReplaceTagError::OldImageNotTagged:
        std::fprintf(stderr, "pzt tag replace: '%s' 目前没有这个标签\n", args[2].c_str());
        break;
      case pzt::core::ReplaceTagError::NewImageNotFound:
        std::fprintf(stderr, "pzt tag replace: 找不到新图片 '%s'\n", args[3].c_str());
        break;
    }
    return 1;
  }
  std::printf("已用 '%s' 替换 '%s' 在标签 '%s' 下的位置\n", args[3].c_str(), args[2].c_str(),
              args[1].c_str());
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
  const std::string& output_folder = args[2];

  auto link_mode = pzt::core::LinkMode::Copy;
  for (std::size_t i = 3; i < args.size(); ++i) {
    if (args[i] == "--link") link_mode = pzt::core::LinkMode::Symlink;
  }

  auto result = pzt::core::export_tag(*tag_id, output_folder, link_mode);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt export: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }

  const auto& r = result.value();
  std::printf("已导出 %d 张到 '%s'", r.exported_count, output_folder.c_str());
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

// current_image_relative_path 参数省略时返回 nullopt (next/prev/next-
// untagged/prev-untagged 各自对"没有当前图片"有明确定义的行为)。
std::optional<pzt::core::ImageId> resolve_optional_current(pzt::core::ProjectId project_id,
                                                            const std::vector<std::string>& args,
                                                            std::size_t index) {
  if (args.size() <= index) return std::nullopt;
  return pzt::core::find_image_by_path(project_id, args[index]);
}

const pzt::core::ImageRef* find_ref(const std::vector<pzt::core::ImageRef>& images,
                                     pzt::core::ImageId id) {
  for (const auto& r : images) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

void print_nav_result(const std::vector<pzt::core::ImageRef>& images,
                      std::optional<pzt::core::ImageId> result, const char* none_message) {
  if (images.empty()) {
    std::printf("项目内没有图片\n");
    return;
  }
  if (!result) {
    std::printf("%s\n", none_message);
    return;
  }
  const auto* ref = find_ref(images, *result);
  std::printf("%s\n", ref ? ref->file_path.c_str() : "(内部错误:找不到结果图片)");
}

int browse_next(const std::vector<std::string>& args, bool prev) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt browse %s: 缺少 <project_name>\n", prev ? "prev" : "next");
    print_browse_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt browse", args[0]);
  if (!project_id) return 1;

  auto images = pzt::core::list_images(*project_id);
  auto current = resolve_optional_current(*project_id, args, 1);
  auto result = prev ? pzt::core::prev_image(images, current) : pzt::core::next_image(images, current);
  print_nav_result(images, result, "");
  return 0;
}

int browse_untagged(const std::vector<std::string>& args, bool prev) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt browse %s-untagged: 缺少 <project_name>\n", prev ? "prev" : "next");
    print_browse_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt browse", args[0]);
  if (!project_id) return 1;

  auto images = pzt::core::list_images(*project_id);
  auto current = resolve_optional_current(*project_id, args, 1);
  auto result = prev ? pzt::core::prev_untagged(images, current)
                      : pzt::core::next_untagged(images, current);
  print_nav_result(images, result, "没有更多未打标签的图片了");
  return 0;
}

int browse_filter(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "pzt browse filter: 缺少 <project_name> <tag_name>\n");
    print_browse_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt browse filter", args[0]);
  if (!project_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[1]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt browse filter: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }

  auto result = pzt::core::filter_by_tag(*tag_id);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt browse filter: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }
  if (result.value().empty()) {
    std::printf("(这个标签下还没有图片)\n");
    return 0;
  }
  for (const auto& ref : result.value()) {
    std::printf("%s\n", ref.file_path.c_str());
  }
  return 0;
}

int cmd_browse(const std::vector<std::string>& args) {
  if (args.empty()) {
    print_browse_usage();
    return 1;
  }
  const std::string& verb = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  if (verb == "next") return browse_next(rest, false);
  if (verb == "prev") return browse_next(rest, true);
  if (verb == "next-untagged") return browse_untagged(rest, false);
  if (verb == "prev-untagged") return browse_untagged(rest, true);
  if (verb == "filter") return browse_filter(rest);

  std::fprintf(stderr, "pzt browse: 未知子命令 '%s'\n", verb.c_str());
  print_browse_usage();
  return 1;
}

// 临时调试命令:解码一张 JPEG,打印尺寸和像素字节数就退出,用来验证
// core/decode 管线本身是通的。increment 6.2/6.4 会把解码结果接到 Kitty
// 渲染器和全键盘循环里，届时这条命令就可以退休了。
int cmd_decode(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt decode: missing <jpeg_path>\n");
    print_usage();
    return 1;
  }
  auto result = pzt::core::decode_jpeg_file(args[0]);
  if (!result.ok()) {
    switch (result.error()) {
      case pzt::core::DecodeError::FileNotFound:
        std::fprintf(stderr, "pzt decode: 找不到文件 '%s'\n", args[0].c_str());
        break;
      case pzt::core::DecodeError::DecodeFailed:
        std::fprintf(stderr, "pzt decode: '%s' 不是可解码的图像\n", args[0].c_str());
        break;
    }
    return 1;
  }
  const auto& img = result.value();
  std::printf("解码成功: %dx%d,%zu 字节 RGBA\n", img.width, img.height, img.rgba.size());
  return 0;
}

// 临时调试命令:解码一张 JPEG 并用 Kitty 图形协议渲染到终端就退出,用来验证
// cli/kitty 渲染管线本身是通的(t=t 传输介质 + Tmux DCS passthrough 检
// 测)。increment 6.4 全键盘循环接上真正的浏览渲染路径之后,这条命令就可以
// 退休了。人类可读的状态信息全部打到 stderr,stdout 只承载真正发给终端的
// Kitty 协议字节,不能与状态信息混流(与 spikes/kitty_latency_probe/ 的约
// 定一致)。
int cmd_render(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt render: missing <jpeg_path>\n");
    print_usage();
    return 1;
  }

  auto decoded = pzt::core::decode_jpeg_file(args[0]);
  if (!decoded.ok()) {
    switch (decoded.error()) {
      case pzt::core::DecodeError::FileNotFound:
        std::fprintf(stderr, "pzt render: 找不到文件 '%s'\n", args[0].c_str());
        break;
      case pzt::core::DecodeError::DecodeFailed:
        std::fprintf(stderr, "pzt render: '%s' 不是可解码的图像\n", args[0].c_str());
        break;
    }
    return 1;
  }

  auto mode = pzt::cli::kitty::detect_terminal_mode();
  if (mode.inside_tmux) {
    std::fprintf(stderr, "[pzt render] 运行在 Tmux 窗格内,allow-passthrough=%s\n",
                 mode.passthrough_ok ? "on" : "off");
  } else {
    std::fprintf(stderr, "[pzt render] 运行在独立 Ghostty 窗口(不在 Tmux 内)\n");
  }

  std::string tmp_path = pzt::cli::kitty::make_tmp_path(std::to_string(getpid()));
  auto result = pzt::cli::kitty::render_rgba_via_tmpfile(STDOUT_FILENO, mode, decoded.value(),
                                                          /*image_id=*/1, tmp_path);
  if (!result.ok()) {
    switch (result.error()) {
      case pzt::cli::kitty::RenderError::PassthroughDisabled:
        std::fprintf(stderr,
                     "pzt render: 当前 Tmux 会话未开启 allow-passthrough,Kitty 图形协议无法穿透"
                     "到 Ghostty。请在 tmux.conf 里加 `set -g allow-passthrough on` 后重启会话,"
                     "或在独立 Ghostty 窗口(不经过 Tmux)里直接运行\n");
        break;
      case pzt::cli::kitty::RenderError::WriteFailed:
        std::fprintf(stderr, "pzt render: 写入终端失败\n");
        break;
    }
    return 1;
  }

  std::fprintf(stderr, "[pzt render] 已发送 %dx%d 像素到终端(image_id=1,tmp_path=%s)\n",
               decoded.value().width, decoded.value().height, tmp_path.c_str());
  return 0;
}

// 临时调试命令:沿浏览顺序走一遍项目里的全部图片,每一步都调用 core/browse
// 的预取缓存 set_current() + get(),验证预取窗口能跟着导航移动、后台解码线
// 程确实在被 get() 阻塞等待之前就把像素准备好。真正接入全键盘循环是
// increment 6.4 的事,这里只验证 core/browse 预取模块本身是通的,人类可读
// 的每步结果打到 stdout,延迟日志(hit/miss/decode 耗时)由 core/browse 自
// 己打到 stderr。
int cmd_prefetch(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt prefetch: missing <project_name>\n");
    print_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt prefetch", args[0]);
  if (!project_id) return 1;

  std::size_t window = 2;
  if (args.size() >= 2) window = static_cast<std::size_t>(std::atoll(args[1].c_str()));

  auto proj = pzt::core::open_project(*project_id);
  if (!proj.ok()) {
    std::fprintf(stderr, "pzt prefetch: 找不到项目 '%s'\n", args[0].c_str());
    return 1;
  }

  auto images = pzt::core::list_images(*project_id);
  if (images.empty()) {
    std::printf("(项目内没有图片)\n");
    return 0;
  }

  std::printf("预取窗口 window=%zu,项目共 %zu 张图片\n", window, images.size());
  pzt::core::PrefetchCache cache(proj.value().root_path, window);

  std::optional<pzt::core::ImageId> current = images.front().id;
  for (std::size_t step = 0; step < images.size(); ++step) {
    cache.set_current(images, current);
    const auto* ref = find_ref(images, *current);
    auto fetched = cache.get(*current);
    if (fetched.ok()) {
      std::printf("[%zu/%zu] %s -> %dx%d,%zu 字节 RGBA\n", step + 1, images.size(),
                  ref->file_path.c_str(), fetched.value().width, fetched.value().height,
                  fetched.value().rgba.size());
    } else {
      const char* reason = fetched.error() == pzt::core::FetchError::NotInWindow
                                ? "not_in_window"
                                : "decode_failed";
      std::printf("[%zu/%zu] %s -> 预取失败(%s)\n", step + 1, images.size(),
                  ref->file_path.c_str(), reason);
    }
    current = pzt::core::next_image(images, current);
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

  if (verb == "create") return tag_create(rest);
  if (verb == "list") return tag_list(rest);
  if (verb == "add") return tag_add(rest);
  if (verb == "remove") return tag_remove(rest);
  if (verb == "replace") return tag_replace(rest);

  std::fprintf(stderr, "pzt tag: 未知子命令 '%s'\n", verb.c_str());
  print_tag_usage();
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
  if (subcommand == "decode") return cmd_decode(args);
  if (subcommand == "render") return cmd_render(args);
  if (subcommand == "prefetch") return cmd_prefetch(args);
  if (subcommand == "tag") return cmd_tag(args);
  if (subcommand == "browse") return cmd_browse(args);

  std::fprintf(stderr, "pzt: 未知子命令 '%s'\n", subcommand.c_str());
  print_usage();
  return 1;
}
