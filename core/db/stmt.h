#pragma once

#include <sqlite3.h>

#include <stdexcept>
#include <string>

// Small internal SQLite helpers shared across core's DAO-style modules
// (core/project, core/tagging, ...). Not part of core/api.h's public facade.
namespace pzt::core::db {

// Thin RAII wrapper so early-return error paths can't leak a live statement.
class Stmt {
 public:
  Stmt(sqlite3* conn, const char* sql) {
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("prepare failed: ") + sqlite3_errmsg(conn));
    }
  }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  sqlite3_stmt* get() const { return stmt_; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

inline void exec_simple(sqlite3* conn, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(conn, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string message = err ? err : "unknown sqlite error";
    sqlite3_free(err);
    throw std::runtime_error(message);
  }
}

}  // namespace pzt::core::db
