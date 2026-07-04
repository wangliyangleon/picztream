#pragma once

#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

// 终端尺寸探测 + 备用屏幕缓冲区。见 docs/M0_Eng_Design.md increment 6.4.2。
namespace pzt::cli::term {

// 同时拿 cell 尺寸(行/列,布局用)和像素尺寸(ws_xpixel/ws_ypixel,算图片
// 缩放比例用)。不是 tty,或者终端没上报像素尺寸(部分老终端/非图形终端
// 会把 xpixel/ypixel 报成 0)时 valid=false,调用方应该有一个合理的兜底。
struct TerminalSize {
  int cols = 0;
  int rows = 0;
  int pixel_width = 0;
  int pixel_height = 0;
  bool valid = false;
};

inline TerminalSize get_terminal_size(int fd = STDOUT_FILENO) {
  TerminalSize size;
  winsize ws{};
  if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0 && ws.ws_xpixel > 0 &&
      ws.ws_ypixel > 0) {
    size.cols = ws.ws_col;
    size.rows = ws.ws_row;
    size.pixel_width = ws.ws_xpixel;
    size.pixel_height = ws.ws_ypixel;
    size.valid = true;
  }
  return size;
}

namespace detail {
inline void write_all(int fd, const char* s) { write(fd, s, std::strlen(s)); }
}  // namespace detail

// 备用屏幕缓冲区 + 隐藏光标的 RAII,跟 CbreakMode 一样的安全原则:构造时
// 进入(切到备用缓冲区、隐藏光标),析构时无条件退出(恢复光标、切回主缓
// 冲区),不管从哪条路径退出。这是解决"上一帧光标停在哪不可控,下一帧从
// 哪开始画也不可控"的关键——没有这个,连续渲染会互相踩踏。只在 fd 是 tty
// 时才生效,非 tty 时构造/析构都是 no-op。
class AltScreen {
 public:
  explicit AltScreen(int fd = STDOUT_FILENO) : fd_(fd) {
    if (!isatty(fd_)) return;
    detail::write_all(fd_, "\x1b[?1049h\x1b[?25l");
    active_ = true;
  }

  ~AltScreen() {
    if (active_) detail::write_all(fd_, "\x1b[?25h\x1b[?1049l");
  }

  AltScreen(const AltScreen&) = delete;
  AltScreen& operator=(const AltScreen&) = delete;

 private:
  int fd_;
  bool active_ = false;
};

}  // namespace pzt::cli::term
