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

// `PRAGMA <name>;`(不带参数)是查询当前值的标准写法。这两个 helper 让下
// 面几个用例可以直接对一条开出来的连接断言 pragma 的实际取值。
int read_int_pragma(sqlite3* conn, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  int value = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

std::string read_text_pragma(sqlite3* conn, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  const unsigned char* text = sqlite3_column_text(stmt, 0);
  std::string value = text ? reinterpret_cast<const char*>(text) : "";
  sqlite3_finalize(stmt);
  return value;
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

// T-7：schema 版本闸门。SQLite 给每个库留了一个 4 字节的 user_version
// 供应用自己用,默认 0。PZT 把"当前 schema"定为 1,于是"读到 0"同时覆盖
// 了全新空文件和任何 T-7 之前建的老库,两者走同一条全量初始化路径,跑完
// 盖章;读到 1 就直接跳过全部建表与加列检查。
TEST_CASE("a freshly created database is stamped at the current schema version") {
  std::string path = temp_db_path("schema_version_fresh");

  auto db = pzt::core::db::Database::open_at(path);
  CHECK(read_int_pragma(db.handle(), "PRAGMA user_version;") == 1);
}

TEST_CASE("reopening an already-stamped database leaves user_version unchanged") {
  std::string path = temp_db_path("schema_version_reopen");

  { auto db1 = pzt::core::db::Database::open_at(path); }
  auto db2 = pzt::core::db::Database::open_at(path);
  CHECK(read_int_pragma(db2.handle(), "PRAGMA user_version;") == 1);
}

// T-7：WAL。默认的 rollback journal 下写者阻塞读者,而 PZT 现在确实是多
// 写者(EvaluationWorker 后台 jthread 一条连接 + 主线程 + agent 每图派生
// 的 pzt 子进程)。
TEST_CASE("opening a database enables WAL journal mode") {
  std::string path = temp_db_path("wal_mode");

  auto db = pzt::core::db::Database::open_at(path);
  CHECK(read_text_pragma(db.handle(), "PRAGMA journal_mode;") == "wal");
}

// journal_mode 是持久化在库文件里的,不是每条连接的设置,这条用例钉住
// 这个前提,因为开库路径正是靠它才能"先读一次,已经是 wal 就不再设"。
TEST_CASE("WAL journal mode persists across close and reopen") {
  std::string path = temp_db_path("wal_persists");

  { auto db1 = pzt::core::db::Database::open_at(path); }
  auto db2 = pzt::core::db::Database::open_at(path);
  CHECK(read_text_pragma(db2.handle(), "PRAGMA journal_mode;") == "wal");
}
