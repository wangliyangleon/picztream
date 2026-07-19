#include "cli/ui/ui.h"

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>

#include "cli/text/text.h"

// pad_to / utf8_continuation_bytes 来自 cli/text,用 using-directive 让下
// 面搬过来的函数体保持逐字不变(.cpp 里用 using,头文件里绝不用)。
using namespace pzt::cli::text;

namespace pzt::cli::ui {

void write_stdout(const std::string& s) { write(STDOUT_FILENO, s.data(), s.size()); }

void move_cursor(int row, int col) {
  write_stdout("\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H");
}

// 画一条横线(边框顶/底/分隔线用):起止两端用 left_char/right_char,如果
// mid_offset >= 0,在这一列(相对 start_col 的偏移,0 是最左边框那一列)插
// 入 mid_char(跟竖直分隔线交汇的地方),其余位置用 "─" 填满。宽度按显示列
// 数算,不是字节数——box-drawing 字符在 UTF-8 里是多字节,这里始终整存整
// 取一个字符,不做字节级切片。
void draw_hline(int row, int start_col, int width, const std::string& left_char,
                const std::string& right_char, int mid_offset, const std::string& mid_char) {
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
void draw_vlines(int row, int start_col, int width, int mid_offset) {
  move_cursor(row, start_col);
  write_stdout("│");  // │
  if (mid_offset >= 0) {
    move_cursor(row, start_col + mid_offset);
    write_stdout("│");
  }
  move_cursor(row, start_col + width - 1);
  write_stdout("│");
}

// 两层菜单(选标签、cap 超限选替换对象)都只需要读一个字节就能得出最终结
// 果,不需要为"取消"单独过滤——EOF/出错和 Esc(0x1B)一样当取消处理。
char read_one_byte() {
  char c = 0;
  ssize_t n = read(STDIN_FILENO, &c, 1);
  return n <= 0 ? 0x1B : c;
}

// 几乎所有单层菜单(space/g/r 顶层及各自的子选择)都是同一套尾巴:把拼
// 好的提示行画到 banner 那一行,再读一个字节。抽出来去掉重复的
// move_cursor+pad_to+write_stdout+read_one_byte 四连击。
char prompt_and_read_key(const std::string& line, int banner_row, int start_col,
                          int content_cols) {
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line, content_cols));
  return read_one_byte();
}

// space/g/r 顶层菜单选项一多(标签/预设列表长了)容易把字母/Esc 这些固定
// 操作挤到看不见的地方——两行版本,line1(带编号的选项)/line2(字母/Esc)
// 分开画,再读一个字节,语义跟单行版本一样。
char prompt_and_read_key_2line(const std::string& line1, const std::string& line2, int banner_row,
                                int start_col, int content_cols) {
  move_cursor(banner_row, start_col + 1);
  write_stdout(pad_to(line1, content_cols));
  move_cursor(banner_row + 1, start_col + 1);
  write_stdout(pad_to(line2, content_cols));
  return read_one_byte();
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

void flush_pending_input() { tcflush(STDIN_FILENO, TCIFLUSH); }

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
namespace {

enum class LineEditResult { Continue, Submit, Cancel };

// 光标感知的单步编辑：读一个字节(方向键要为了跟裸 Esc 消歧再多读几
// 个)，更新 buffer/cursor，返回这一步的结果。read_text_line/
// read_text_line_with_placeholder 共用——两者的差异只在 redraw(有没有
// placeholder、要不要两行换行)，编辑状态机完全一样，不值得分别写一遍。
//
// 左右方向键是 "\x1b" "[" "C"/"D" 三字节序列，前缀跟裸 Esc(0x1B)撞车
// ——原来的实现直接把任何 0x1B 都当成取消，按左方向键会被误判成 Esc，
// 直接退出整个输入(真实反馈过的 bug)。用 stdin_ready 探测紧跟着有没有
// 更多字节区分两者：本地 pty 场景下，终端一次性把整个序列写进来，后续
// 字节几乎立即可读；真正单独按 Esc 不会有紧跟着的字节。20ms 是经验
// 值，给终端一点余量又不会让用户感觉到延迟。探测到是转义序列但不认识
// 具体是哪个键(方向键之外的功能键/Alt+字符)时，直接吞掉、什么都不做
// ——"其它键处理不了的就无事发生"，不是也当成 Esc 处理掉。
LineEditResult read_line_edit_step(std::string& buffer, std::size_t& cursor, int& pending_needed) {
  char c = read_one_byte();
  if (c == 0x1B) {
    if (!stdin_ready(20)) return LineEditResult::Cancel;  // 裸 Esc,没有紧跟着的字节
    char c2 = read_one_byte();
    if (c2 == '[') {
      char c3 = read_one_byte();
      if (c3 == 'D') {  // 左
        if (cursor > 0) {
          std::size_t pos = cursor - 1;
          while (pos > 0 && (static_cast<unsigned char>(buffer[pos]) & 0xC0) == 0x80) --pos;
          cursor = pos;
        }
      } else if (c3 == 'C') {  // 右
        if (cursor < buffer.size()) {
          std::size_t pos = cursor + 1;
          while (pos < buffer.size() && (static_cast<unsigned char>(buffer[pos]) & 0xC0) == 0x80) ++pos;
          cursor = pos;
        }
      }
      // 其它 CSI 序列(Home/End/Delete/功能键...)这次不识别，吞掉已经读
      // 到的这几个字节就算处理完，不做任何事。
    }
    // 不是 '[' 开头的序列(比如 Alt+字符)，同样不认识，吞掉这一个字节。
    return LineEditResult::Continue;
  }
  if (c == '\r' || c == '\n') return LineEditResult::Submit;
  if (c == 0x7F || c == 0x08) {
    if (cursor > 0) {
      std::size_t pos = cursor - 1;
      while (pos > 0 && (static_cast<unsigned char>(buffer[pos]) & 0xC0) == 0x80) --pos;
      buffer.erase(pos, cursor - pos);
      cursor = pos;
    }
    pending_needed = 0;
    return LineEditResult::Continue;
  }
  if (static_cast<unsigned char>(c) < 0x20) return LineEditResult::Continue;  // 其它控制字节,忽略

  // 普通字节:插入到光标位置(不是永远追加到末尾)。UTF-8 续字节紧跟着上
  // 一个字节插入,光标顺着往后移,插入点天然保持连续,不需要特殊处理。
  buffer.insert(cursor, 1, c);
  ++cursor;
  if (pending_needed > 0) {
    --pending_needed;
  } else {
    pending_needed = utf8_continuation_bytes(static_cast<unsigned char>(c));
  }
  return LineEditResult::Continue;
}

}  // namespace

std::optional<std::string> read_text_line(const std::string& prompt, int banner_row,
                                           int start_col, int content_cols) {
  std::string buffer;
  std::size_t cursor = 0;
  int pending_needed = 0;  // 还差几个续字节才能凑成当前码点

  // F-42：超宽内容会用到第二行(见下面 redraw 的换行处理),而 banner_row+1
  // 平时可能杵着其它提示("q:[退出]"/菜单),先清干净,跟
  // read_text_line_with_placeholder 一样,不然长路径换行时会跟旧提示串行。
  move_cursor(banner_row + 1, start_col + 1);
  write_stdout(pad_to("", content_cols));

  auto redraw = [&] {
    // F-42:内容(常驻 prompt + 已输入 buffer)超出第一行宽度时换到第二行,
    // 跟 read_text_line_with_placeholder 的两行逻辑同构——区别只是这里的固定
    // 前缀是常驻 prompt,而不是那边的一个前导空格。以前是单行 pad,长导出路
    // 径输入时光标会越出右边框。
    std::string content = prompt + buffer;
    std::string line1 = truncate_text(content, static_cast<std::size_t>(content_cols));
    std::string line2 =
        truncate_text(content.substr(line1.size()), static_cast<std::size_t>(content_cols));
    move_cursor(banner_row, start_col + 1);
    write_stdout(pad_to(line1, content_cols));
    move_cursor(banner_row + 1, start_col + 1);
    write_stdout(pad_to(line2, content_cols));

    // 光标跟着 cursor(buffer 内字节偏移)走,对应 content 里的字节位是
    // prompt.size() + cursor;超出第一行时跟着换到第二行。
    std::size_t cursor_byte_pos = prompt.size() + cursor;
    if (cursor_byte_pos <= line1.size()) {
      std::string up_to_cursor = content.substr(0, cursor_byte_pos);
      move_cursor(banner_row, start_col + 1 + static_cast<int>(display_width(up_to_cursor)));
    } else {
      std::string rest = content.substr(line1.size());
      std::size_t rest_cursor_pos = cursor_byte_pos - line1.size();
      std::string up_to_cursor_line2 = rest.substr(0, std::min(rest_cursor_pos, line2.size()));
      move_cursor(banner_row + 1, start_col + 1 + static_cast<int>(display_width(up_to_cursor_line2)));
    }
  };
  write_stdout("\x1b[?25h");
  redraw();

  while (true) {
    auto result = read_line_edit_step(buffer, cursor, pending_needed);
    if (result == LineEditResult::Cancel) {
      write_stdout("\x1b[?25l");
      return std::nullopt;
    }
    if (result == LineEditResult::Submit) {
      write_stdout("\x1b[?25l");
      return buffer;
    }
    if (pending_needed == 0) redraw();
  }
}

// M3：跟 read_text_line 共用 read_line_edit_step，唯一的区别是 redraw：
// 没有常驻前缀，buffer 为空时显示 placeholder，一旦开始输入 placeholder
// 整个让位给 buffer 本身。
std::optional<std::string> read_text_line_with_placeholder(const std::string& placeholder,
                                                             int banner_row, int start_col,
                                                             int content_cols) {
  std::string buffer;
  std::size_t cursor = 0;
  int pending_needed = 0;

  // 显示内容统一加一个前导空格,跟其它 banner 提示(比如" 新标签名称: "、
  // " AI 处理中，请稍后 ")的留白风格一致,不然这一行会紧贴左边框,显得跟别
  // 处不一样。第二行(banner_row+1)顶层空闲状态时是"q:[退出]"——这个函数
  // 只用一行的话那行提示会在输入过程中一直杵在那,跟正在输入的内容不搭,
  // 先清空;超出第一行宽度的内容也会用到这一行(见 redraw 里的换行处理),
  // 不管哪种情况都需要先清干净。
  move_cursor(banner_row + 1, start_col + 1);
  write_stdout(pad_to("", content_cols));

  auto redraw = [&] {
    std::string display_content = " " + (buffer.empty() ? placeholder : buffer);
    std::string line1 = truncate_text(display_content, static_cast<std::size_t>(content_cols));
    std::string line2 = truncate_text(display_content.substr(line1.size()),
                                       static_cast<std::size_t>(content_cols));
    move_cursor(banner_row, start_col + 1);
    write_stdout(pad_to(line1, content_cols));
    move_cursor(banner_row + 1, start_col + 1);
    write_stdout(pad_to(line2, content_cols));

    // 光标位置跟着 cursor(buffer 内的字节偏移)走,不理会 placeholder
    // ——哪怕当前画面上显示的是 placeholder,光标也应该停在"即将开始输
    // 入"的位置,不会被更长的 placeholder 文案推到后面去。前导空格占一
    // 个字节,所以光标对应的字节位置是 1 + cursor;内容超出第一行宽度
    // 时光标跟着换到第二行。
    std::string cursor_content = " " + buffer;
    std::size_t cursor_byte_pos = 1 + cursor;
    std::string cursor_line1 = truncate_text(cursor_content, static_cast<std::size_t>(content_cols));
    if (cursor_byte_pos <= cursor_line1.size()) {
      std::string up_to_cursor = cursor_content.substr(0, cursor_byte_pos);
      move_cursor(banner_row, start_col + 1 + static_cast<int>(display_width(up_to_cursor)));
    } else {
      std::string rest = cursor_content.substr(cursor_line1.size());
      std::size_t rest_cursor_pos = cursor_byte_pos - cursor_line1.size();
      std::string cursor_line2 = truncate_text(rest, static_cast<std::size_t>(content_cols));
      std::string up_to_cursor_line2 = rest.substr(0, std::min(rest_cursor_pos, cursor_line2.size()));
      move_cursor(banner_row + 1, start_col + 1 + static_cast<int>(display_width(up_to_cursor_line2)));
    }
  };

  // AltScreen 进入时把光标整个隐藏了(浏览图片期间没有意义的光标位置),这
  // 里是唯一需要用户看清"输入到哪了"的地方,临时显示,函数返回前(不管是
  // Esc 取消还是 Enter 提交)都要还原,不能让光标一直显示着回到浏览状态。
  write_stdout("\x1b[?25h");
  redraw();

  while (true) {
    auto result = read_line_edit_step(buffer, cursor, pending_needed);
    if (result == LineEditResult::Cancel) {
      write_stdout("\x1b[?25l");
      return std::nullopt;
    }
    if (result == LineEditResult::Submit) {
      write_stdout("\x1b[?25l");
      return buffer;
    }
    if (pending_needed == 0) redraw();
  }
}

}  // namespace pzt::cli::ui
