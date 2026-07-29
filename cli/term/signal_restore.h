#pragma once

#include <csignal>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <termios.h>
#include <unistd.h>

// 信号路径上的终端还原。
//
// CbreakMode/AltScreen 都是 RAII,正常退出和异常退出都能还原终端——
// main.cpp 顶层那个兜底 catch 就是专门为后者准备的。但默认的信号处置是
// 直接终止进程、不做栈回退,析构函数根本没有机会执行:Ctrl-C 下去,
// AltScreen 的 \x1b[?25h\x1b[?1049l 永远写不出来,用户被留在备用屏幕
// 里、光标还是隐藏的(termios 一般会被 shell 自己重置,备用屏幕和隐藏光
// 标不会有人替你还原)。而 cbreak 模式是刻意保留 ISIG 的(见
// cbreak_mode.h),把 Ctrl-C 当作正式的退路——既然推荐了这条路,它就得跟
// 正常退出一样干净。开了 /dedup --ai 之后阻塞窗口从几十秒拉到分钟级,
// 伸手去按 Ctrl-C 的概率大幅上升,这条路更要紧。
//
// 做法:两个 RAII 类构造时把"该怎么还原"登记进来、析构时撤销登记,信号
// 处理函数只做最少的事——按析构相反的顺序还原,然后把处置改回默认、重
// 新 raise 一次。重新 raise 而不是 exit(),是为了保持原本的退出语义:
// shell 看到的仍然是"被 SIGINT 杀死",退出码和 core dump 行为都不变。
//
// 处理函数里只调用 async-signal-safe 的函数(write/tcsetattr/signal/
// raise,都在 POSIX 的名单里),不碰 iostream、不分配内存、不加锁。
//
// 假设同一时刻每种状态最多只有一个实例:cmd_open 一处、cmd_recipe 的交
// 互流程一处,两者不嵌套。登记是覆盖式的,不做引用计数。
namespace pzt::cli::term::signal_restore {

namespace detail {

// volatile sig_atomic_t:这几个标志会被信号处理函数读到。fd 和 termios
// 本体只在 armed 为真时才被读,而 armed 是最后一个被设置的(见 arm_*),
// 所以处理函数看到 armed 为真时它们必然已经写好了。
inline volatile sig_atomic_t termios_armed = 0;
inline int termios_fd = -1;
inline termios termios_original{};

inline volatile sig_atomic_t altscreen_armed = 0;
inline int altscreen_fd = -1;

inline volatile sig_atomic_t handlers_installed = 0;

// 取消作用域(见下面 CancelScope)。active 为真时 SIGINT 不再终止进程,只
// 置位 requested 并把 echo 缓冲区原样吐到终端上。
//
// echo 必须是预先渲染好的字节:按下 Ctrl-C 的那一刻主线程正阻塞在 curl 或
// 解码里,没有任何人能替它重画,只能由处理函数自己写。而处理函数里不能拼
// i18n、不能分配内存、不能碰 std::string——所以整行(含光标定位转义序列)在
// 进作用域时就拷进这个定长缓冲区。
constexpr std::size_t kEchoCapacity = 4096;
inline volatile sig_atomic_t cancel_active = 0;
inline volatile sig_atomic_t cancel_requested = 0;
inline char cancel_echo[kEchoCapacity] = {};
inline std::size_t cancel_echo_len = 0;
inline int cancel_echo_fd = -1;

}  // namespace detail

// 还原顺序跟 cmd_open 里的析构顺序一致:先把输入模式还原,再离开备用缓
// 冲区。每种状态还原后立即撤销登记,重复调用是安全的(第二次什么都不
// 做),正常退出路径上析构函数已经还原过时也不会重复写。
inline void restore_now() {
  if (detail::termios_armed) {
    detail::termios_armed = 0;
    tcsetattr(detail::termios_fd, TCSANOW, &detail::termios_original);
  }
  if (detail::altscreen_armed) {
    detail::altscreen_armed = 0;
    static const char kLeave[] = "\x1b[?25h\x1b[?1049l";
    // 忽略返回值:信号处理函数里已经无处报错,而且此刻进程正在退出。
    ssize_t ignored = write(detail::altscreen_fd, kLeave, sizeof(kLeave) - 1);
    (void)ignored;
  }
}

// extern "C" 是给 signal() 用的;名字带全前缀是因为 extern "C" 的链接名
// 不受 namespace 约束,会占用全局 C 符号。
extern "C" inline void pzt_cli_term_on_fatal_signal(int sig) {
  // 取消作用域内的 SIGINT 不终止进程:置位 + 回显,然后原样返回,让主线程
  // 在下一个检查点自己收手。只拦 SIGINT——SIGTERM/SIGHUP/SIGQUIT 不是
  // "用户在 TUI 里按了 Ctrl-C",没有理由被降级成一次业务取消。
  if (sig == SIGINT && detail::cancel_active) {
    detail::cancel_requested = 1;
    if (detail::cancel_echo_len > 0) {
      ssize_t ignored = write(detail::cancel_echo_fd, detail::cancel_echo, detail::cancel_echo_len);
      (void)ignored;
    }
    return;
  }
  restore_now();
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

// 只接管会跳过析构、又确实会在正常使用中撞上的那几个:SIGINT(Ctrl-C)、
// SIGQUIT(Ctrl-\)、SIGHUP(终端关掉)、SIGTERM(被 kill)。不接管
// SIGKILL/SIGSTOP(接不了),也不接管 SIGSEGV/SIGABRT 这类真正的崩溃——
// 那种情况下进程状态已经不可信,还原终端不是首要问题,留给 core dump。
inline void install_handlers_once() {
  if (detail::handlers_installed) return;
  detail::handlers_installed = 1;
  for (int sig : {SIGINT, SIGQUIT, SIGHUP, SIGTERM}) {
    // 先探一次:如果调用方(比如 nohup)显式把这个信号设成了忽略,尊重
    // 它,不要把它改成会终止进程的处理函数。
    if (std::signal(sig, SIG_IGN) == SIG_IGN) continue;
    std::signal(sig, pzt_cli_term_on_fatal_signal);
  }
}

inline void arm_termios(int fd, const termios& original) {
  install_handlers_once();
  detail::termios_fd = fd;
  detail::termios_original = original;
  detail::termios_armed = 1;  // 最后设置:前面两个字段必须先写好
}

inline void disarm_termios() { detail::termios_armed = 0; }

inline void arm_altscreen(int fd) {
  install_handlers_once();
  detail::altscreen_fd = fd;
  detail::altscreen_armed = 1;  // 同上
}

inline void disarm_altscreen() { detail::altscreen_armed = 0; }

// 一段"Ctrl-C 表示取消这次操作，而不是退出程序"的作用域。
//
// 用在 `/dedup` 这种阻塞几分钟的命令上：进作用域后 SIGINT 只置位标志并
// 立刻回显一行提示，调用方在自己的检查点上读 cancelled() 决定收手；出作
// 用域立刻恢复成 signal_restore 的默认语义(还原终端后重新 raise，即 T-9a
// 的"干净退出 pzt")。
//
// **析构必须无条件跑到**，否则 Ctrl-C 会在浏览界面里彻底失效：既不取消
// (没有操作在跑)也不退出(处置被换掉了)，用户按了没反应也退不出去。所以
// 这是 RAII 而不是两个手动调用的函数。
//
// echo_line 是要在按下时吐出去的完整字节序列，含光标定位——调用方按自己
// 的布局拼好(见 handle_dedup_command)。超过缓冲区容量时降级成不回显，
// 而不是截断：截断多字节字符会在终端上吐出半个 UTF-8 序列。
//
// cancelled() 是粘性的，满足 core::dedup::CancelFn 的契约。
class CancelScope {
 public:
  explicit CancelScope(const std::string& echo_line, int echo_fd = STDOUT_FILENO) {
    install_handlers_once();
    detail::cancel_echo_len = 0;
    if (echo_line.size() <= detail::kEchoCapacity) {
      for (std::size_t i = 0; i < echo_line.size(); ++i) detail::cancel_echo[i] = echo_line[i];
      detail::cancel_echo_len = echo_line.size();
    }
    detail::cancel_echo_fd = echo_fd;
    detail::cancel_requested = 0;
    detail::cancel_active = 1;  // 最后置位:前面几个字段必须先写好
  }

  ~CancelScope() {
    detail::cancel_active = 0;
    detail::cancel_echo_len = 0;
  }

  CancelScope(const CancelScope&) = delete;
  CancelScope& operator=(const CancelScope&) = delete;

  bool cancelled() const { return detail::cancel_requested != 0; }
};

}  // namespace pzt::cli::term::signal_restore
