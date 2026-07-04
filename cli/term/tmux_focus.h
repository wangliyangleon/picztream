#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

// tmux 切走当前 pane 时,Kitty 协议画出来的图片残留问题。见
// docs/M0_Eng_Design.md increment 6.4 的 tmux 焦点处理设计。
//
// 根因:Kitty 协议的图片 placement 是叠加在文字网格之上的独立合成层,由底
// 层终端管理,tmux 本身并不理解这一层——即便开了 passthrough 转发字节,
// tmux 切换窗口时只会重绘新窗口的文字内容,不会主动清掉上一个窗口留下的
// 图片。我们的进程这时候还在跑,只是阻塞在 read() 等键,不知道自己被切
// 走了。
//
// 第一版实现用的是 tmux 的 pane-focus-out/pane-focus-in hook,真机测试确
// 认没用——这两个 hook 依赖真实终端级别的焦点事件(`focus-events` 选项打
// 开后,tmux 会向外层终端请求转发的是"这个 OS 窗口有没有失去焦点",不是
// tmux 自己内部"当前显示哪个 window"的状态),在同一个 Ghostty 窗口里切
// tmux 窗口(`next-window`/`previous-window`)根本不会产生真实的终端焦点
// 事件,自然不会触发。`after-next-window`/`after-previous-window` 这类命
// 令 hook 在实测的 tmux 3.7 上也不是有效的 hook 名字。
//
// 改用轮询:后台线程每隔一小段时间用 `tmux display-message -p -t <pane>
// '#{window_active}'` 查一次"我这个 pane 所在的 window 现在是不是正在显
// 示给客户端",状态从 1 变 0 时给自己发 SIGUSR1(相当于失焦),从 0 变 1
// 时发 SIGUSR2(相当于重新获焦)——这两个信号本来就是为了打断主线程阻塞
// 中的 read(),轮询只是换了个更可靠的触发来源,后续处理逻辑不用变。
namespace pzt::cli::term {

namespace detail {
inline std::atomic<bool> g_focus_out_requested{false};
inline std::atomic<bool> g_focus_in_requested{false};

inline void handle_focus_out(int) { g_focus_out_requested.store(true); }
inline void handle_focus_in(int) { g_focus_in_requested.store(true); }

// fork+exec 直接调用 tmux,不经过 /bin/sh,读子进程 stdout 判断
// #{window_active} 是不是 "1"。查询失败(fork/pipe 出错)时保守地当作"还
// 在显示",不误触发清理。
inline bool tmux_window_active(const std::string& pane) {
  int fds[2];
  if (pipe(fds) != 0) return true;

  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("tmux", "tmux", "display-message", "-p", "-t", pane.c_str(), "#{window_active}",
           static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return true;
  }
  close(fds[1]);
  char buf[8] = {0};
  ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return n > 0 && buf[0] == '1';
}

}  // namespace detail

class TmuxFocusWatcher {
 public:
  explicit TmuxFocusWatcher(bool inside_tmux) : active_(inside_tmux) {
    if (!active_) return;

    const char* pane = std::getenv("TMUX_PANE");
    pane_ = pane ? pane : "";
    if (pane_.empty()) {
      active_ = false;
      return;
    }

    struct sigaction sa_out {};
    sa_out.sa_handler = detail::handle_focus_out;
    sigemptyset(&sa_out.sa_mask);
    sa_out.sa_flags = 0;  // 不设 SA_RESTART:必须能打断阻塞中的 read()
    sigaction(SIGUSR1, &sa_out, nullptr);

    struct sigaction sa_in {};
    sa_in.sa_handler = detail::handle_focus_in;
    sigemptyset(&sa_in.sa_mask);
    sa_in.sa_flags = 0;
    sigaction(SIGUSR2, &sa_in, nullptr);

    poller_ = std::jthread([this](std::stop_token stop) { poll_loop(stop); });
  }

  TmuxFocusWatcher(const TmuxFocusWatcher&) = delete;
  TmuxFocusWatcher& operator=(const TmuxFocusWatcher&) = delete;

  // 主循环在 read() 被 EINTR 打断之后调用,取走(并清空)对应的标记。
  static bool consume_focus_out() { return detail::g_focus_out_requested.exchange(false); }
  static bool consume_focus_in() { return detail::g_focus_in_requested.exchange(false); }

 private:
  void poll_loop(std::stop_token stop) {
    bool was_active = true;  // 刚打开时假设自己就是当前正在显示的窗口
    const int kPollMs = 300;
    const int kCheckMs = 50;  // 拆成小段睡,保证 request_stop 后能很快退出
    int waited_ms = kPollMs;  // 让第一轮循环立刻查一次

    while (!stop.stop_requested()) {
      if (waited_ms >= kPollMs) {
        bool now_active = detail::tmux_window_active(pane_);
        if (was_active && !now_active) {
          kill(getpid(), SIGUSR1);
        } else if (!was_active && now_active) {
          kill(getpid(), SIGUSR2);
        }
        was_active = now_active;
        waited_ms = 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kCheckMs));
      waited_ms += kCheckMs;
    }
  }

  bool active_;
  std::string pane_;
  std::jthread poller_;
};

}  // namespace pzt::cli::term
