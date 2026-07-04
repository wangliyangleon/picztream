#pragma once

#include <termios.h>
#include <unistd.h>

// cbreak 模式的 RAII 包装。没有叫 RawMode——"RAW" 在这个项目里另有所指
// (相机 RAW 格式,M2 的核心议题),叫 raw 容易和 RAW 解码混到一起。这里实
// 际做的配置(关 ICANON/ECHO、保留 ISIG)在经典 Unix 终端编程术语里本来就
// 有更准确的名字——cbreak 模式,跟真正禁掉一切信号处理的 raw 模式是两回
// 事。
//
// 构造时切换到 cbreak(读键不用等回车、不回显;保留 ISIG,Ctrl+C 依旧能
// 强制终止程序,不用自己另外处理信号),析构时无条件还原成原始设置——不
// 管从哪条路径退出(正常退出、异常),都不会把用户的终端留在一个"看不见
// 输入、回车不换行"的坏状态。只在 fd 确实是一个 tty 时才生效,非 tty(比
// 如被重定向到文件)时构造/析构都是 no-op,不报错。
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
    if (tcsetattr(fd_, TCSANOW, &raw) == 0) active_ = true;
  }

  ~CbreakMode() {
    if (active_) tcsetattr(fd_, TCSANOW, &original_);
  }

  CbreakMode(const CbreakMode&) = delete;
  CbreakMode& operator=(const CbreakMode&) = delete;

 private:
  int fd_;
  termios original_{};
  bool active_ = false;
};

}  // namespace pzt::cli::term
