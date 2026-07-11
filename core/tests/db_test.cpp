#include <doctest.h>

#include <filesystem>
#include <string>

#include "core/db/database.h"

namespace {

// Each test gets its own throwaway DB path under the OS temp dir so tests
// never touch the real ~/.config/pzt/pzt.db and don't collide with each
// other.
std::string temp_db_path(const std::string& tag) {
  auto dir = std::filesystem::temp_directory_path() / "pzt_test";
  std::filesystem::create_directories(dir);
  return (dir / (tag + ".db")).string();
}

}  // namespace

TEST_CASE("opening a fresh database creates it and initializes schema") {
  std::string path = temp_db_path("fresh_schema");
  std::filesystem::remove(path);

  auto db = pzt::core::db::Database::open_at(path);
  CHECK(std::filesystem::exists(path));
  CHECK(db.handle() != nullptr);
}

TEST_CASE("schema initialization is idempotent - reopening doesn't fail") {
  std::string path = temp_db_path("reopen_schema");
  std::filesystem::remove(path);

  { auto db1 = pzt::core::db::Database::open_at(path); }
  auto db2 = pzt::core::db::Database::open_at(path);
  CHECK(db2.handle() != nullptr);
}

TEST_CASE("default_db_path respects XDG_CONFIG_HOME") {
  setenv("XDG_CONFIG_HOME", "/tmp/pzt_xdg_test", 1);
  CHECK(pzt::core::db::default_db_path() == "/tmp/pzt_xdg_test/pzt/pzt.db");
  unsetenv("XDG_CONFIG_HOME");
}

// F-04：每条新打开的连接都要设置忙等超时,不能让并发写(比如后台
// EvaluationWorker 的独立连接)一撞上写锁就立刻抛异常。`PRAGMA
// busy_timeout;`(不带参数)是查询当前值的标准写法,直接断言开出来的连
// 接确实带着这个设置,不用真的起两个线程去竞争锁。
TEST_CASE("opening a database sets a non-zero busy_timeout") {
  std::string path = temp_db_path("busy_timeout");
  std::filesystem::remove(path);

  auto db = pzt::core::db::Database::open_at(path);
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), "PRAGMA busy_timeout;", -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  int timeout_ms = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  CHECK(timeout_ms > 0);
}
