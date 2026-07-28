#include "core/db/database.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "core/db/schema.h"

namespace pzt::core::db {

namespace {

// 默认的 rollback journal 下写者阻塞读者,而 PZT 现在确实是多写者:
// EvaluationWorker 的后台 jthread 一条连接、主线程一条、agent 每张图还
// 会派生一个 pzt 子进程(agent/stages/curate.py、style_apply_all.py)。
// 今天靠上面那个 5 秒忙等兜着,WAL 让读写不再互相阻塞。
//
// journal_mode 是持久化在库文件里的,不是每条连接的设置,所以先读一次、
// 已经是 wal 就不用再设,这也避免了在一个我们可能马上要拒绝打开的库上
// 做无谓的写入。
//
// 必须用 prepare/step 而不是 sqlite3_exec:这条 pragma 会返回一行"实际
// 生效的模式",而且可能返回 SQLITE_OK 却没切成功(瞬时锁竞争,或者库在一
// 个不支持共享内存的文件系统上),只看返回码不够。
//
// 切不过去就静默算了,不抛:WAL 是性能与并发上的改进,不是正确性前提,
// 没有理由因为它让整个 pzt 打不开库。
void enable_wal_if_needed(sqlite3* db) {
  auto read_journal_mode = [&]() -> std::string {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &stmt, nullptr) != SQLITE_OK) return "";
    std::string mode;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char* text = sqlite3_column_text(stmt, 0);
      if (text) mode = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(stmt);
    for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return mode;
  };

  if (read_journal_mode() == "wal") return;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA journal_mode = WAL;", -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_step(stmt);  // 返回的那一行就是切换后的模式,切不过去也不是错误
  sqlite3_finalize(stmt);
}

}  // namespace

std::string default_db_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::filesystem::path config_home =
      (xdg && *xdg) ? std::filesystem::path(xdg)
                    : std::filesystem::path(std::getenv("HOME")) / ".config";
  return (config_home / "pzt" / "pzt.db").string();
}

Database Database::open_default() { return open_at(default_db_path()); }

Database Database::open_at(const std::string& path) {
  std::filesystem::path p(path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path());
  }

  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    std::string message = db ? sqlite3_errmsg(db) : "unknown error";
    if (db) sqlite3_close(db);
    throw std::runtime_error("failed to open database at " + path + ": " + message);
  }

  // M3 引入了后台线程(EvaluationWorker)在独立连接上写库,跟主线程/其它
  // 进程的写操作并发时,SQLite 默认的空 busy handler 会让锁冲突立刻返回
  // SQLITE_BUSY,core 里所有 DAO 遇到非 SQLITE_DONE 都是 throw,这会让一
  // 次短暂的锁等待变成一次异常(在 worker 线程里未捕获就是 std::terminate,
  // 见 F-05 的说明)。给每条打开的连接设置几秒的忙等超时,让 SQLite 自己
  // 在这个时间窗口内重试,而不是立刻报错,这是 SQLite 官方推荐的多连接
  // 并发写法,不需要额外的锁或队列。
  sqlite3_busy_timeout(db, 5000);

  // initialize_schema 抛出时 db 还是个裸 handle,Database 的构造函数还没
  // 跑,RAII 接管不了,直接抛就是永久泄漏。以前 initialize_schema 只在
  // "不该发生"的场景抛(建表失败、库损坏),所以这个洞一直没被注意到;T-7
  // 之后"库的 schema 版本比程序新"是一条常规路径(SchemaTooNewError),抛
  // 出不再是意外,泄漏就得堵上。
  try {
    // WAL 要在 initialize_schema 之前:v0 迁移整段跑在一个事务里,而
    // journal_mode 不能在事务内切换。
    enable_wal_if_needed(db);
    initialize_schema(db);
  } catch (...) {
    sqlite3_close(db);
    throw;
  }
  return Database(db, path);
}

Database::~Database() {
  if (db_) sqlite3_close(db_);
}

Database::Database(Database&& other) noexcept
    : db_(other.db_), path_(std::move(other.path_)) {
  other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    if (db_) sqlite3_close(db_);
    db_ = other.db_;
    path_ = std::move(other.path_);
    other.db_ = nullptr;
  }
  return *this;
}

}  // namespace pzt::core::db
