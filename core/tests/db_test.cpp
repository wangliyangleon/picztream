#include <doctest.h>

#include <filesystem>
#include <string>

#include "core/db/database.h"
#include "core/db/schema.h"

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

// 下面几个用例要模拟"老库"和"被外部改坏的库",都得绕开 Database::open_at
// (那会直接跑最新的 initialize_schema)、直接对裸 handle 动手,所以需要这
// 几个小工具。
void raw_exec(sqlite3* conn, const char* sql) {
  REQUIRE(sqlite3_exec(conn, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
}

bool table_present(sqlite3* conn, const std::string& table) {
  sqlite3_stmt* stmt = nullptr;
  std::string sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name='" + table + "';";
  REQUIRE(sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  bool found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

bool column_present(sqlite3* conn, const std::string& table, const std::string& column) {
  sqlite3_stmt* stmt = nullptr;
  std::string sql = "PRAGMA table_info(" + table + ");";
  REQUIRE(sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  bool found = false;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* name = sqlite3_column_text(stmt, 1);
    if (name && column == reinterpret_cast<const char*>(name)) found = true;
  }
  sqlite3_finalize(stmt);
  return found;
}

int scalar_int(sqlite3* conn, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  int value = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

// 一个项目 + 一张图,用来验证迁移没有碰这些表里的数据。
void seed_project_and_image(sqlite3* conn) {
  raw_exec(conn,
            "INSERT INTO projects (id, name, root_path, created_at) "
            "VALUES (1, 'p', '/root', 100);");
  raw_exec(conn,
            "INSERT INTO images (id, project_id, file_path, file_name, file_size, imported_at) "
            "VALUES (1, 1, 'a.jpg', 'a.jpg', 10, 100);");
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
  CHECK(read_int_pragma(db.handle(), "PRAGMA user_version;") == pzt::core::db::kSchemaVersion);
  // 单独把字面量也钉一次:上面那条只保证"盖的章跟常量一致",将来 bump
  // 常量时它会自动跟着变;这条会红,提醒 bump 是一次有意的改动。
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

// 这条是唯一真正钉住"快路径"的用例,上面那两个版本号用例在没有快路径
// (每次都全量跑一遍 DDL)的实现下也会通过。手动删掉一张表但把版本号留在
// 1,如果 initialize_schema 还在无条件跑 CREATE TABLE IF NOT EXISTS,表会
// 被重新建出来。
TEST_CASE("initialize_schema skips all DDL when the database is already stamped") {
  std::string path = temp_db_path("schema_fast_path");

  { auto db = pzt::core::db::Database::open_at(path); }

  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  raw_exec(raw, "DROP TABLE tags;");
  REQUIRE(!table_present(raw, "tags"));
  REQUIRE(scalar_int(raw, "PRAGMA user_version;") == pzt::core::db::kSchemaVersion);

  pzt::core::db::initialize_schema(raw);
  CHECK(!table_present(raw, "tags"));

  sqlite3_close(raw);
}

// T-7 之前的二进制建的库长这样:结构是现代的,但没有版本号(默认 0)。迁移
// 要把它盖章成 1,且一行数据都不能碰,尤其是 image_evaluations,它的结构
// 已经是现代形态,结构校验不该误伤它。
TEST_CASE("a legacy v0 database with the modern schema is stamped without losing data") {
  std::string path = temp_db_path("schema_v0_modern");

  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  pzt::core::db::initialize_schema(raw);  // 先建出完整的现代结构
  raw_exec(raw, "PRAGMA user_version = 0;");  // 再退回"没有版本号"的状态
  seed_project_and_image(raw);
  raw_exec(raw,
            "INSERT INTO image_evaluations (image_id, result_json, extra_guidance, provider) "
            "VALUES (1, '{\"assessment\":\"ok\",\"unusable\":false}', '', 'local');");

  pzt::core::db::initialize_schema(raw);

  CHECK(scalar_int(raw, "PRAGMA user_version;") == pzt::core::db::kSchemaVersion);
  CHECK(scalar_int(raw, "SELECT COUNT(*) FROM projects;") == 1);
  CHECK(scalar_int(raw, "SELECT COUNT(*) FROM images;") == 1);
  CHECK(scalar_int(raw, "SELECT COUNT(*) FROM image_evaluations;") == 1);

  sqlite3_close(raw);
}

// W2026-07-21 之前的 image_evaluations 是另一套列。那种表里存的东西跟现
// 在的读法对不上,必须 drop 重建;但只有这张表该被牺牲,其它表的数据一行
// 都不能少。
TEST_CASE("a legacy v0 database with the old image_evaluations shape gets that table rebuilt") {
  std::string path = temp_db_path("schema_v0_old_eval");

  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  pzt::core::db::initialize_schema(raw);
  raw_exec(raw, "DROP TABLE image_evaluations;");
  raw_exec(raw,
            "CREATE TABLE image_evaluations ("
            "  image_id   INTEGER PRIMARY KEY,"
            "  assessment TEXT NOT NULL,"
            "  unusable   INTEGER NOT NULL"
            ");");
  raw_exec(raw, "PRAGMA user_version = 0;");
  seed_project_and_image(raw);
  raw_exec(raw, "INSERT INTO image_evaluations (image_id, assessment, unusable) VALUES (1, 'x', 0);");
  REQUIRE(!column_present(raw, "image_evaluations", "result_json"));

  pzt::core::db::initialize_schema(raw);

  CHECK(column_present(raw, "image_evaluations", "result_json"));
  CHECK(scalar_int(raw, "SELECT COUNT(*) FROM image_evaluations;") == 0);
  CHECK(scalar_int(raw, "SELECT COUNT(*) FROM projects;") == 1);
  CHECK(scalar_int(raw, "SELECT COUNT(*) FROM images;") == 1);
  CHECK(scalar_int(raw, "PRAGMA user_version;") == pzt::core::db::kSchemaVersion);

  sqlite3_close(raw);
}

// 全新安装的形态:版本 0 且 image_evaluations 还不存在。结构校验必须先问
// "表在不在",否则会走进"表存在但缺列"的分支。旧实现是靠 column_exists 在
// 缺表时也返回 false 这个巧合躲过去的。
TEST_CASE("a v0 database without an image_evaluations table is left alone") {
  std::string path = temp_db_path("schema_v0_no_eval");

  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  REQUIRE(scalar_int(raw, "PRAGMA user_version;") == 0);
  REQUIRE(!table_present(raw, "image_evaluations"));

  pzt::core::db::initialize_schema(raw);

  CHECK(table_present(raw, "image_evaluations"));
  CHECK(column_present(raw, "image_evaluations", "result_json"));
  CHECK(scalar_int(raw, "PRAGMA user_version;") == pzt::core::db::kSchemaVersion);

  sqlite3_close(raw);
}

// 降级防护:装过更新版的 pzt、又装回旧版。这种库的结构未知,拒绝打开而不
// 是尽力而为。
TEST_CASE("opening a database stamped at a newer schema version throws SchemaTooNewError") {
  std::string path = temp_db_path("schema_too_new");

  { auto db = pzt::core::db::Database::open_at(path); }

  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  raw_exec(raw, "PRAGMA user_version = 99;");
  sqlite3_close(raw);

  CHECK_THROWS_AS(pzt::core::db::Database::open_at(path), pzt::core::db::SchemaTooNewError);
  // "拒绝打开"必须真的不碰这个库:版本号原样,也没有被顺手切成 WAL。
  // (open_at 里 enable_wal_if_needed 排在 initialize_schema 之后就是为
  // 了这个,journal_mode 是要写进库文件头的。)
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  CHECK(scalar_int(raw, "PRAGMA user_version;") == 99);
  sqlite3_close(raw);
  // cli 的顶层 main() 有一条兜底的 catch (const std::exception&),这条断言
  // 钉住"专用类型没被 catch 到时仍会落进兜底"这个继承关系。
  CHECK_THROWS_AS(pzt::core::db::Database::open_at(path), std::runtime_error);
}
