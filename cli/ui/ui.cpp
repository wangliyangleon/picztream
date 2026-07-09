#include "cli/ui/ui.h"

#include <poll.h>
#include <unistd.h>

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

}  // namespace pzt::cli::ui
