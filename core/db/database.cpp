#include "core/db/database.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#include "core/db/schema.h"

namespace pzt::core::db {

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
  // SQLITE_BUSY——core 里所有 DAO 遇到非 SQLITE_DONE 都是 throw,这会让一
  // 次短暂的锁等待变成一次异常(在 worker 线程里未捕获就是 std::terminate,
  // 见 F-05 的说明)。给每条打开的连接设置几秒的忙等超时,让 SQLite 自己
  // 在这个时间窗口内重试,而不是立刻报错——这是 SQLite 官方推荐的多连接
  // 并发写法,不需要额外的锁或队列。
  sqlite3_busy_timeout(db, 5000);

  initialize_schema(db);
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
