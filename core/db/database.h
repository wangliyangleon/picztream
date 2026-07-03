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

 private:
  explicit Database(sqlite3* db) : db_(db) {}

  sqlite3* db_;
};

}  // namespace pzt::core::db
