#pragma once

#include <deque>
#include <fcntl.h>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// 把 stderr 重定向出去,不让 core 层(比如 PrefetchCache)的延迟日志原样跟
// 图片画到同一个 pty 上,把画面弄乱。见 docs/M0_Eng_Design.md increment 6.4
// 的调试面板设计。
namespace pzt::cli::term {

// 未开启(默认路径)时只是把 stderr 重定向到 /dev/null,不起后台线程——不
// 需要看这些日志时,不产生任何额外开销。开启(`pzt open --debug`)时改成
// 重定向到一个内部管道,后台 jthread 把读到的内容按行存进一个环形缓冲区,
// 供全键盘循环每帧读一份快照、画到屏幕底部专门的 debug 区域。
class DebugLogRedirect {
 public:
  DebugLogRedirect(bool enabled, std::size_t max_lines) : enabled_(enabled), max_lines_(max_lines) {
    saved_stderr_ = dup(STDERR_FILENO);
    if (saved_stderr_ < 0) return;

    if (!enabled_) {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
      return;
    }

    int fds[2];
    if (pipe(fds) != 0) return;
    read_fd_ = fds[0];
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    reader_ = std::jthread([this](std::stop_token stop) { reader_loop(stop); });
  }

  ~DebugLogRedirect() {
    if (saved_stderr_ < 0) return;
    // dup2 隐式关掉当前 stderr 指向的写端(管道模式下,这会让后台线程阻塞
    // 的 read() 收到 EOF 自然退出;/dev/null 模式下就是单纯换回原来的 fd)。
    dup2(saved_stderr_, STDERR_FILENO);
    close(saved_stderr_);
    if (reader_.joinable()) reader_.join();
    if (read_fd_ >= 0) close(read_fd_);
  }

  DebugLogRedirect(const DebugLogRedirect&) = delete;
  DebugLogRedirect& operator=(const DebugLogRedirect&) = delete;

  std::vector<std::string> snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::vector<std::string>(lines_.begin(), lines_.end());
  }

 private:
  void reader_loop(std::stop_token stop) {
    std::string buf;
    char chunk[256];
    while (!stop.stop_requested()) {
      ssize_t n = read(read_fd_, chunk, sizeof(chunk));
      if (n <= 0) break;  // EOF(管道写端已关闭)或出错,退出
      buf.append(chunk, static_cast<std::size_t>(n));
      std::size_t pos;
      while ((pos = buf.find('\n')) != std::string::npos) {
        push_line(buf.substr(0, pos));
        buf.erase(0, pos + 1);
      }
    }
  }

  void push_line(std::string line) {
    std::lock_guard<std::mutex> lock(mu_);
    lines_.push_back(std::move(line));
    while (lines_.size() > max_lines_) lines_.pop_front();
  }

  bool enabled_;
  std::size_t max_lines_;
  int saved_stderr_ = -1;
  int read_fd_ = -1;
  mutable std::mutex mu_;
  std::deque<std::string> lines_;
  std::jthread reader_;
};

}  // namespace pzt::cli::term
