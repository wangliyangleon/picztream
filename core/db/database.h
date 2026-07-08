#pragma once

#include <sqlite3.h>

#include <string>

// Thin RAII wrapper around the connection to PZT's global metadata database.
// See docs/M0_Eng_Design.md 待确认问题 for why this is a single global DB
// (~/.config/pzt/pzt.db) rather than one file per project folder: list/open/
// archive/delete all operate by project name across the whole installation,
// independent of cwd.
namespace pzt::core::db {

// Resolves the default DB path without opening it (respects
// $XDG_CONFIG_HOME, falls back to ~/.config). Exposed for cli/test
// introspection.
std::string default_db_path();

class Database {
 public:
  // Opens (creating the file and parent directory if needed) the global DB
  // at its default location, running schema initialization.
  static Database open_default();

  // Opens at an explicit path, running schema initialization. Used by tests
  // to avoid touching the real user database.
  static Database open_at(const std::string& path);

  ~Database();
  Database(Database&& other) noexcept;
  Database& operator=(Database&& other) noexcept;
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  sqlite3* handle() const { return db_; }

  // M2：RAW 预览缓存文件也要落在跟这个数据库同一个"数据目录"下（同一父目
  // 录），而不是硬编码 ~/.config/pzt/——测试用 open_at 指向 /tmp 下的临时
  // db 时，缓存也必须跟着落进 /tmp，不能污染真实的 ~/.config/pzt/。
  const std::string& path() const { return path_; }

 private:
  Database(sqlite3* db, std::string path) : db_(db), path_(std::move(path)) {}

  sqlite3* db_;
  std::string path_;
};

}  // namespace pzt::core::db
