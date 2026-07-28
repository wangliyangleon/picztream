#include <doctest.h>

#include <filesystem>
#include <string>

#include "core/db/database.h"

namespace {

// Each test gets its own throwaway DB path under the OS temp dir so tests
// never touch the real ~/.config/pzt/pzt.db and don't collide with each
// other.
//
// 删除动作在 helper 里而不是各个调用点:开了 WAL 之后每次开库都会在主文
// 件旁边生成 <path>-wal 和 <path>-shm,而这个目录是跨轮复用的。只删主文
// 件的话,下一轮 sqlite3_open 会在一个零字节库旁边发现同名的、上一轮留
// 下的活 WAL 并对它做恢复,"fresh" 库就不 fresh 了,可能读到陈旧内容或
// 直接报损坏。core/tests 里另外 9 个 fresh_db_path/temp_db_path 出于同
// 样的理由各自也删这两个边车。
std::string temp_db_path(const std::string& tag) {
  auto dir = std::filesystem::temp_directory_path() / "pzt_test";
  std::filesystem::create_directories(dir);
  auto path = (dir / (tag + ".db")).string();
  std::filesystem::remove(path);
  std::filesystem::remove(path + "-wal");
  std::filesystem::remove(path + "-shm");
  return path;
}

}  // namespace

TEST_CASE("opening a fresh database creates it and initializes schema") {
  std::string path = temp_db_path("fresh_schema");

  auto db = pzt::core::db::Database::open_at(path);
  CHECK(std::filesystem::exists(path));
  CHECK(db.handle() != nullptr);
}

TEST_CASE("schema initialization is idempotent - reopening doesn't fail") {
  std::string path = temp_db_path("reopen_schema");

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

  auto db = pzt::core::db::Database::open_at(path);
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), "PRAGMA busy_timeout;", -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  int timeout_ms = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  CHECK(timeout_ms > 0);
}
