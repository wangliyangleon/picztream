#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "cli/term/debug_log.h"

using pzt::cli::term::DebugLogRedirect;

namespace {

// 后台读线程是异步的,不能写完就断言。轮询到超时,通过时几毫秒就返回。
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

bool snapshot_contains(const DebugLogRedirect& log, const std::string& needle) {
  for (const auto& line : log.snapshot()) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// on_lines_appended 存在的唯一理由:`/dedup` 这类同步阻塞命令跑的时候主循
// 环停着,没人重画 debug 面板,日志要等命令整个跑完才一次性刷出来。AI 比较
// 尤其难受——prompt 是在 http_post 之前打的,本该让人看着"这次在比哪两
// 张",结果要等响应回来才显示。所以这里断言的核心性质是"对象还活着的时候
// 就回调了",不是"析构时补一次"。

TEST_CASE("DebugLogRedirect: on_lines_appended fires while the redirect is still alive") {
  DebugLogRedirect log(/*enabled=*/true, /*max_lines=*/32);
  std::atomic<int> calls{0};
  log.set_on_lines_appended([&](const std::vector<std::string>&) { ++calls; });

  std::fprintf(stderr, "[pzt ai] request (local) prompt: which one\n");
  std::fflush(stderr);

  CHECK(wait_until([&] { return calls.load() > 0; }));
  CHECK(snapshot_contains(log, "which one"));
}

TEST_CASE("DebugLogRedirect: each flush of new lines triggers another repaint") {
  DebugLogRedirect log(/*enabled=*/true, /*max_lines=*/32);
  std::atomic<int> calls{0};
  log.set_on_lines_appended([&](const std::vector<std::string>&) { ++calls; });

  // 一次比较会打两条(请求一条、响应一条),中间隔着几十秒的 http_post。两
  // 条各自触发一次重画,面板才会"发出去的时候就显示 prompt"。
  std::fprintf(stderr, "request\n");
  std::fflush(stderr);
  CHECK(wait_until([&] { return calls.load() >= 1; }));
  int after_first = calls.load();

  std::fprintf(stderr, "response\n");
  std::fflush(stderr);
  CHECK(wait_until([&] { return calls.load() > after_first; }));
  CHECK(snapshot_contains(log, "response"));
}

TEST_CASE("DebugLogRedirect: clearing the callback stops the repaints") {
  DebugLogRedirect log(/*enabled=*/true, /*max_lines=*/32);
  std::atomic<int> calls{0};
  log.set_on_lines_appended([&](const std::vector<std::string>&) { ++calls; });

  std::fprintf(stderr, "first\n");
  std::fflush(stderr);
  REQUIRE(wait_until([&] { return calls.load() > 0; }));

  // 摘掉之后必须真的不再画:常挂着的话,主循环画整帧(含 Kitty 图片传输那
  // 种大块转义序列)时会跟它抢 stdout,插进去半行就能把图片写坏。
  log.set_on_lines_appended(nullptr);
  int frozen = calls.load();

  std::fprintf(stderr, "second\n");
  std::fflush(stderr);
  REQUIRE(wait_until([&] { return snapshot_contains(log, "second"); }));  // 行还是收进去了
  CHECK(calls.load() == frozen);                                          // 但没有再回调
}

TEST_CASE("DebugLogRedirect: disabled mode never calls back") {
  DebugLogRedirect log(/*enabled=*/false, /*max_lines=*/32);
  std::atomic<int> calls{0};
  log.set_on_lines_appended([&](const std::vector<std::string>&) { ++calls; });

  // 不开 --debug 时 stderr 去 /dev/null、根本没有读线程,注册了也不该有任
  // 何动静(browse.cpp 那边靠 log=nullptr 走同一条 no-op 路径)。
  std::fprintf(stderr, "ignored\n");
  std::fflush(stderr);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(calls.load() == 0);
  CHECK(log.snapshot().empty());
}
