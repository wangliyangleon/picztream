#include <doctest.h>

#include <filesystem>
#include <string>

#include "core/db/database.h"
#include "core/db/schema.h"
#include "core/recipe/recipe.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using namespace pzt::core::recipe;

namespace {

std::string temp_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / (tag + ".db")).string();
  fs::remove(path);
  return path;
}

}  // namespace

TEST_CASE("ensure_default_presets seeds presets and is idempotent") {
  auto db = Database::open_at(temp_db_path("recipe_seed"));

  ensure_default_presets(db);
  auto first = list_presets(db);
  REQUIRE(first.size() == 2);

  ensure_default_presets(db);
  auto second = list_presets(db);
  CHECK(second.size() == first.size());
  CHECK(second[0].id == first[0].id);
  CHECK(second[1].id == first[1].id);
}

TEST_CASE("list_presets returns presets in creation order") {
  auto db = Database::open_at(temp_db_path("recipe_order"));
  ensure_default_presets(db);

  auto presets = list_presets(db);
  REQUIRE(presets.size() == 2);
  CHECK(presets[0].id < presets[1].id);
  CHECK(presets[0].name == "Standard");
  CHECK(presets[1].name == "Warm");
}

TEST_CASE("list_versions returns empty for any preset (no creation entry point yet)") {
  auto db = Database::open_at(temp_db_path("recipe_versions_empty"));
  ensure_default_presets(db);
  auto presets = list_presets(db);
  REQUIRE(!presets.empty());

  CHECK(list_versions(db, presets[0].id).empty());
  CHECK(list_versions(db, 999999).empty());  // 不存在的 preset_id 同样返回空,不报错
}

TEST_CASE("initialize_schema migrates a pre-M1 database by adding images.recipe_id") {
  std::string path = temp_db_path("recipe_migration");

  // 手动搭一个 M0 时代的 images 表(没有 recipes 表、没有 recipe_id 列),
  // 不走 Database::open_at(那会直接跑最新的 initialize_schema),模拟"老
  // 库第一次遇到 M1 代码"这个场景。
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  REQUIRE(sqlite3_exec(raw,
                        "CREATE TABLE images (id INTEGER PRIMARY KEY, project_id INTEGER NOT "
                        "NULL, file_path TEXT NOT NULL, file_name TEXT NOT NULL, file_size "
                        "INTEGER NOT NULL, imported_at INTEGER NOT NULL);",
                        nullptr, nullptr, nullptr) == SQLITE_OK);

  auto column_present = [&]() {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(raw, "PRAGMA table_info(images);", -1, &stmt, nullptr);
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char* name = sqlite3_column_text(stmt, 1);
      if (name && std::string(reinterpret_cast<const char*>(name)) == "recipe_id") found = true;
    }
    sqlite3_finalize(stmt);
    return found;
  };

  REQUIRE(!column_present());
  pzt::core::db::initialize_schema(raw);
  CHECK(column_present());

  // 幂等:再跑一次不报错,列还在。
  pzt::core::db::initialize_schema(raw);
  CHECK(column_present());

  sqlite3_close(raw);
}
