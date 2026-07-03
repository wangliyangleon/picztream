#include <doctest.h>

#include <filesystem>
#include <string>

#include "core/db/database.h"

namespace {

// Each test gets its own throwaway DB path under the OS temp dir so tests
// never touch the real ~/.config/pzt/pzt.db and don't collide with each
// other.
std::string temp_db_path(const std::string& tag) {
  auto dir = std::filesystem::temp_directory_path() / "pzt_test";
  std::filesystem::create_directories(dir);
  return (dir / (tag + ".db")).string();
}

}  // namespace

TEST_CASE("opening a fresh database creates it and initializes schema") {
  std::string path = temp_db_path("fresh_schema");
  std::filesystem::remove(path);

  auto db = pzt::core::db::Database::open_at(path);
  CHECK(std::filesystem::exists(path));
  CHECK(db.handle() != nullptr);
}

TEST_CASE("schema initialization is idempotent - reopening doesn't fail") {
  std::string path = temp_db_path("reopen_schema");
  std::filesystem::remove(path);

  { auto db1 = pzt::core::db::Database::open_at(path); }
  auto db2 = pzt::core::db::Database::open_at(path);
  CHECK(db2.handle() != nullptr);
}

TEST_CASE("default_db_path respects XDG_CONFIG_HOME") {
  setenv("XDG_CONFIG_HOME", "/tmp/pzt_xdg_test", 1);
  CHECK(pzt::core::db::default_db_path() == "/tmp/pzt_xdg_test/pzt/pzt.db");
  unsetenv("XDG_CONFIG_HOME");
}
