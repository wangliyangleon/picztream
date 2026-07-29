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
