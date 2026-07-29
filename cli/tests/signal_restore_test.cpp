#include <doctest.h>

#include <csignal>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "cli/term/signal_restore.h"

using namespace pzt::cli::term::signal_restore;

// T-9a：信号路径上的终端还原。真正的端到端行为(Ctrl-C 之后终端是否干
// 净)只能在真机上验,这里覆盖的是可测的那部分——登记/撤销登记的状态机
// 与写出的字节序列。信号处理函数本身不在这里触发:它最后会把处置改回
// SIG_DFL 再 raise,会直接杀掉测试进程。
namespace {

// 建一对管道,读端设成非阻塞——"什么都没写"是这里好几个用例要断言的结
// 果,阻塞式 read 撞上空管道会直接挂死。
struct Pipe {
  int read_fd = -1;
  int write_fd = -1;

  Pipe() {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    read_fd = fds[0];
    write_fd = fds[1];
    REQUIRE(fcntl(read_fd, F_SETFL, O_NONBLOCK) == 0);
  }

  ~Pipe() {
    close(read_fd);
    close(write_fd);
  }

  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  // 把已经写进去的内容读出来。测试里写的量远小于管道缓冲区,一次读得完。
  std::string drain() {
    char buf[64];
    ssize_t n = read(read_fd, buf, sizeof(buf));
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
  }
};

// 每个用例开头都清一遍:这些是进程级的全局状态,用例之间会互相看见。
void reset_arming() {
  disarm_termios();
  disarm_altscreen();
}

// doctest 自己给 SIGINT 装了处理函数(用来把信号报告成"test case
// CRASHED"),会盖掉 install_handlers_once 装的那个。生产路径上没有这个竞
// 争——只有我们装——所以这是纯粹的测试宿主适配:raise 之前把我们的处理函
// 数显式装回去。
void reinstall_our_sigint_handler() { std::signal(SIGINT, pzt_cli_term_on_fatal_signal); }

}  // namespace

TEST_CASE("restore_now writes the alt-screen exit sequence exactly once") {
  reset_arming();
  Pipe p;

  arm_altscreen(p.write_fd);
  restore_now();
  // 恢复光标 + 切回主缓冲区,顺序与 AltScreen 析构里写的一致。
  CHECK(p.drain() == "\x1b[?25h\x1b[?1049l");

  // 幂等:已经还原过了就不该再写第二遍。正常退出路径上析构函数先还原、
  // 再撤销登记,靠的就是这个性质。
  restore_now();
  CHECK(p.drain().empty());
}

TEST_CASE("disarm_altscreen makes restore_now a no-op") {
  reset_arming();
  Pipe p;

  arm_altscreen(p.write_fd);
  disarm_altscreen();
  restore_now();
  CHECK(p.drain().empty());
}

TEST_CASE("restore_now with nothing armed writes nothing") {
  reset_arming();
  Pipe p;

  restore_now();
  CHECK(p.drain().empty());
}

TEST_CASE("arming installs handlers for the signals that skip destructors") {
  reset_arming();
  Pipe p;
  arm_altscreen(p.write_fd);

  // 装过之后这四个信号都不该还停在 SIG_DFL——默认处置会直接终止进程、
  // 跳过 AltScreen/CbreakMode 的析构,那正是这条改动要堵的洞。
  // 用 signal() 探测会顺手改掉处置,所以探完立刻按原值装回去。
  for (int sig : {SIGINT, SIGQUIT, SIGHUP, SIGTERM}) {
    auto previous = std::signal(sig, SIG_IGN);
    CHECK(previous != SIG_DFL);
    std::signal(sig, previous);
  }

  reset_arming();
}

TEST_CASE("termios arming is independent of alt-screen arming") {
  reset_arming();
  Pipe p;

  // 管道不是 tty,tcsetattr 会失败——这里要的就是"失败也不写任何东西到
  // altscreen 的 fd 上",两种状态互不牵连。cmd_recipe 的交互流程只有
  // CbreakMode、没有 AltScreen,走的就是这条组合。
  termios dummy{};
  arm_termios(p.write_fd, dummy);
  restore_now();
  CHECK(p.drain().empty());

  reset_arming();
}

// T-9b：CancelScope。真机行为(按 Ctrl-C 之后 /dedup 停下来)只能人工验，
// 这里覆盖状态机——作用域内外的 SIGINT 处置、粘性、回显字节。
// 用 raise(SIGINT) 直接触发：作用域内的处理函数不再终止进程，可以安全地
// 在测试进程里跑。

TEST_CASE("CancelScope: SIGINT inside the scope sets the flag instead of killing the process") {
  reset_arming();
  Pipe p;

  reinstall_our_sigint_handler();
  CancelScope scope("cancelling", p.write_fd);
  CHECK_FALSE(scope.cancelled());

  std::raise(SIGINT);  // 作用域内不终止进程——能跑到下一行本身就是断言

  CHECK(scope.cancelled());
  CHECK(p.drain() == "cancelling");
}

TEST_CASE("CancelScope: the flag is sticky") {
  reset_arming();
  Pipe p;

  reinstall_our_sigint_handler();
  CancelScope scope("x", p.write_fd);
  std::raise(SIGINT);
  // core::dedup::CancelFn 要求粘性:内部在分簇每张图、AI 每次比较、两阶段
  // 之间各查一次，中间任何一次翻回 false 都会让流程继续跑下去。
  CHECK(scope.cancelled());
  CHECK(scope.cancelled());
  CHECK(scope.cancelled());
}

TEST_CASE("CancelScope: a fresh scope starts uncancelled") {
  reset_arming();
  Pipe p;

  reinstall_our_sigint_handler();
  {
    CancelScope first("x", p.write_fd);
    std::raise(SIGINT);
    CHECK(first.cancelled());
  }
  // 上一次取消不该渗进下一条命令——否则用户取消过一次 /dedup 之后，再跑
  // 一次会立刻自我取消。
  CancelScope second("x", p.write_fd);
  CHECK_FALSE(second.cancelled());
}

TEST_CASE("CancelScope: leaving the scope restores the terminating disposition") {
  reset_arming();
  Pipe p;
  CHECK(detail::cancel_active == 0);
  {
    CancelScope scope("x", p.write_fd);
    CHECK(detail::cancel_active == 1);
  }

  // 出了作用域,处理函数必须重新走终止那条路。装错或漏还原的话，Ctrl-C 在
  // 浏览界面里既不取消(没有操作在跑)也不退出(处置被换掉了)，用户按了没反
  // 应也退不出去——这是本条特性最容易翻车的地方。
  //
  // cancel_active 就是处理函数里那个分支条件本身(见
  // pzt_cli_term_on_fatal_signal)，所以这一行断言的正是"下一次 SIGINT 会
  // 走 restore_now + 重新 raise"。不能真的 raise 来验——那会按设计杀掉测
  // 试进程。
  CHECK(detail::cancel_active == 0);
  // 回显缓冲区也清了：不清的话下一次强杀路径上会吐出一行上次的残留文案。
  CHECK(detail::cancel_echo_len == 0);
}

TEST_CASE("CancelScope: an over-long echo line degrades to no echo rather than a truncated one") {
  reset_arming();
  Pipe p;

  // 截断多字节字符会往终端上吐半个 UTF-8 序列，比不回显更糟。
  reinstall_our_sigint_handler();
  CancelScope scope(std::string(detail::kEchoCapacity + 1, 'x'), p.write_fd);
  std::raise(SIGINT);
  CHECK(scope.cancelled());  // 取消照常生效，只是没有回显
  CHECK(p.drain().empty());
}
