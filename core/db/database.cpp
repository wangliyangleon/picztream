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

  initialize_schema(db);
  return Database(db);
}

Database::~Database() {
  if (db_) sqlite3_close(db_);
}

Database::Database(Database&& other) noexcept : db_(other.db_) { other.db_ = nullptr; }

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    if (db_) sqlite3_close(db_);
    db_ = other.db_;
    other.db_ = nullptr;
  }
  return *this;
}

}  // namespace pzt::core::db
