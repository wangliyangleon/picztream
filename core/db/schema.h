#pragma once

#include <sqlite3.h>

// Schema for the global PZT metadata database. See docs/M0_Eng_Design.md
// "数据库 Schema 设计" for the authoritative design and rationale.
namespace pzt::core::db {

// Creates all tables (idempotent - CREATE TABLE IF NOT EXISTS) and enables
// foreign key enforcement on this connection. Safe to call on every open.
void initialize_schema(sqlite3* conn);

}  // namespace pzt::core::db
