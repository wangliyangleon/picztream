// Scaffolding smoke tests only: prove the build pipeline actually works
// (SQLite link, doctest vendoring, std::jthread availability). Real module
// tests come in later sessions per docs/M0_Eng_Design.md 任务分解.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <atomic>
#include <sqlite3.h>
#include <stop_token>
#include <string>
#include <thread>

// Deliberately independent of core/api.h's evolving surface - this is a
// scaffolding-level check that the SQLite link itself works, not a test of
// any particular facade function.
TEST_CASE("sqlite is linked and returns a version string") {
  CHECK(std::string(sqlite3_libversion()).size() > 0);
}

TEST_CASE("std::jthread is available and joinable on this toolchain") {
  std::atomic<bool> ran{false};
  {
    // Don't call request_stop() before the thread body runs - that's a race
    // (the thread may observe stop_requested() before doing any work). The
    // jthread destructor requests stop and joins automatically on scope
    // exit, which is enough to prove the type works end to end.
    std::jthread t([&](std::stop_token) { ran = true; });
  }
  CHECK(ran.load());
}
