#pragma once

#include <deque>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// 把 stderr 重定向出去,不让 core 层(比如 PrefetchCache)的延迟日志原样跟
// 图片画到同一个 pty 上,把画面弄乱。见 docs/history/M0_Eng_Design.md increment 6.4
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

  // 收到新日志时回调一次(在后台读线程上跑,读完一整块 read() 里的所有整行
  // 之后调一次,不是每行一次)，参数是回调那一刻的完整快照。
  //
  // 存在的理由只有一个:主循环阻塞在一条同步命令里(`/dedup` 那种)的时候，
  // 没有人再重画 debug 面板——日志明明已经进了环形缓冲区，用户却要等命令
  // 整个跑完才一次性看到。平时不需要这个回调，主循环每帧自己画。
  //
  // **注册期间调用方要自己保证不会跟主线程同时往终端写。** 主循环画整
  // 帧时会往 stdout 吐 Kitty 图片传输那种大块转义序列，插进去半行 debug
  // 面板会直接把图片写坏。所以这是一个用完就摘的东西(见 browse.cpp 的
  // LiveDebugPanel)，不是一直挂着的。
  //
  // 传 nullptr 摘掉时**会等正在跑的那一次回调返回**(靠 cb_mu_)，所以调用
  // 方摘完就可以安全销毁回调捕获的东西。少了这个保证就是 use-after-free:
  // 读线程可能已经把回调拷出来、正要调用，而注册方那边析构已经返回了。
  //
  // 快照当参数传、而不是让回调自己调 snapshot()，是为了让上面那个"等一
  // 次"能成立:回调在 cb_mu_ 下跑，snapshot() 拿的是 mu_，两把锁分开才不
  // 会自己锁死自己。
  void set_on_lines_appended(std::function<void(const std::vector<std::string>&)> fn) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    on_lines_appended_ = std::move(fn);
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
      bool appended = false;
      while ((pos = buf.find('\n')) != std::string::npos) {
        push_line(buf.substr(0, pos));
        buf.erase(0, pos + 1);
        appended = true;
      }
      if (!appended) continue;
      // 先在 mu_ 下取快照,再在 cb_mu_ 下调回调——整个调用都在 cb_mu_ 里,
      // 摘回调的一方才能靠这把锁等到它跑完(见 set_on_lines_appended)。
      auto lines = snapshot();
      std::lock_guard<std::mutex> lock(cb_mu_);
      if (on_lines_appended_) on_lines_appended_(lines);
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
  mutable std::mutex mu_;          // 只护 lines_
  std::deque<std::string> lines_;
  mutable std::mutex cb_mu_;       // 只护回调本身,且回调整个调用都在它下面
  std::function<void(const std::vector<std::string>&)> on_lines_appended_;
  std::jthread reader_;
};

}  // namespace pzt::cli::term
