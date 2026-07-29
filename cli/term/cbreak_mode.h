#pragma once

#include <termios.h>
#include <unistd.h>

#include "cli/term/signal_restore.h"

// cbreak 模式的 RAII 包装。没有叫 RawMode——"RAW" 在这个项目里另有所指
// (相机 RAW 格式,M2 的核心议题),叫 raw 容易和 RAW 解码混到一起。这里实
// 际做的配置(关 ICANON/ECHO、保留 ISIG)在经典 Unix 终端编程术语里本来就
// 有更准确的名字——cbreak 模式,跟真正禁掉一切信号处理的 raw 模式是两回
// 事。
//
// 构造时切换到 cbreak(读键不用等回车、不回显;保留 ISIG,Ctrl+C 依旧能
// 强制终止程序),析构时无条件还原成原始设置——正常退出和异常退出都不会
// 把用户的终端留在一个"看不见输入、回车不换行"的坏状态。只在 fd 确实是
// 一个 tty 时才生效,非 tty(比如被重定向到文件)时构造/析构都是 no-op,
// 不报错。
//
// 但析构函数覆盖不到信号路径:默认信号处置直接终止进程、不做栈回退。保
// 留 ISIG 等于把 Ctrl-C 当作正式的退路,所以那条路要另外接——见
// signal_restore.h,构造时登记、析构时撤销登记。这里说的"另外接"只是还
// 原终端,不是中途取消正在跑的操作(那是提案 T-9b)。
namespace pzt::cli::term {

class CbreakMode {
 public:
  explicit CbreakMode(int fd = STDIN_FILENO) : fd_(fd) {
    if (!isatty(fd_)) return;
    if (tcgetattr(fd_, &original_) != 0) return;

    termios raw = original_;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;   // 阻塞读,直到至少 1 个字节可用
    raw.c_cc[VTIME] = 0;  // 不设超时
    if (tcsetattr(fd_, TCSANOW, &raw) == 0) {
      active_ = true;
      signal_restore::arm_termios(fd_, original_);
    }
  }

  ~CbreakMode() {
    if (!active_) return;
    // 先还原再撤销登记:万一正好在这两步之间来了信号,处理函数会再还原
    // 一次,重复还原是无害的;反过来则会留下一个还原不掉的窗口。
    tcsetattr(fd_, TCSANOW, &original_);
    signal_restore::disarm_termios();
  }

  CbreakMode(const CbreakMode&) = delete;
  CbreakMode& operator=(const CbreakMode&) = delete;

 private:
  int fd_;
  termios original_{};
  bool active_ = false;
};

}  // namespace pzt::cli::term
