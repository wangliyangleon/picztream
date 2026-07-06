#include <doctest.h>

#include <algorithm>
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

TEST_CASE("list_versions returns empty for a preset with no versions created") {
  auto db = Database::open_at(temp_db_path("recipe_versions_empty"));
  ensure_default_presets(db);
  auto presets = list_presets(db);
  REQUIRE(!presets.empty());

  CHECK(list_versions(db, presets[0].id).empty());
  CHECK(list_versions(db, 999999).empty());  // 不存在的 preset_id 同样返回空,不报错
}

TEST_CASE("create_version creates a version under a preset") {
  auto db = Database::open_at(temp_db_path("recipe_create"));
  ensure_default_presets(db);
  auto preset_id = list_presets(db)[0].id;

  VersionParams params{-10, 5, 2, -1};
  auto result = create_version(db, preset_id, std::string("My Look"), params);
  REQUIRE(result.ok());

  auto versions = list_versions(db, preset_id);
  REQUIRE(versions.size() == 1);
  CHECK(versions[0].id == result.value());
  CHECK(versions[0].preset_id == preset_id);
  CHECK(versions[0].name == "My Look");
  CHECK(versions[0].highlights == -10);
  CHECK(versions[0].shadows == 5);
  CHECK(versions[0].wb_shift_r == 2);
  CHECK(versions[0].wb_shift_b == -1);
  CHECK_FALSE(versions[0].deleted);
}

TEST_CASE("create_version allows an unnamed version") {
  auto db = Database::open_at(temp_db_path("recipe_create_unnamed"));
  ensure_default_presets(db);
  auto preset_id = list_presets(db)[0].id;

  auto result = create_version(db, preset_id, std::nullopt, VersionParams{});
  REQUIRE(result.ok());
  auto versions = list_versions(db, preset_id);
  REQUIRE(versions.size() == 1);
  CHECK_FALSE(versions[0].name.has_value());
}

TEST_CASE("create_version reports PresetNotFound for a missing or non-preset id") {
  auto db = Database::open_at(temp_db_path("recipe_create_errors"));
  ensure_default_presets(db);
  auto preset_id = list_presets(db)[0].id;

  auto missing = create_version(db, 999999, std::nullopt, VersionParams{});
  REQUIRE(!missing.ok());
  CHECK(missing.error() == CreateVersionError::PresetNotFound);

  auto version = create_version(db, preset_id, std::nullopt, VersionParams{});
  REQUIRE(version.ok());
  // 传一个 version 的 id 当"预设"用,同样报 PresetNotFound,不是一个新的
  // 错误变体——调用方不需要区分"不存在"和"存在但不是预设"。
  auto nested = create_version(db, version.value(), std::nullopt, VersionParams{});
  REQUIRE(!nested.ok());
  CHECK(nested.error() == CreateVersionError::PresetNotFound);
}

TEST_CASE("rename_version renames a version") {
  auto db = Database::open_at(temp_db_path("recipe_rename"));
  ensure_default_presets(db);
  auto preset_id = list_presets(db)[0].id;
  auto version_id = create_version(db, preset_id, std::string("Old"), VersionParams{}).value();

  auto result = rename_version(db, version_id, "New");
  REQUIRE(result.ok());
  CHECK(list_versions(db, preset_id)[0].name == "New");
}

TEST_CASE("rename_version reports NotFound and IsPreset") {
  auto db = Database::open_at(temp_db_path("recipe_rename_errors"));
  ensure_default_presets(db);
  auto preset_id = list_presets(db)[0].id;

  auto missing = rename_version(db, 999999, "New");
  REQUIRE(!missing.ok());
  CHECK(missing.error() == RecipeOpError::NotFound);

  auto is_preset = rename_version(db, preset_id, "New");
  REQUIRE(!is_preset.ok());
  CHECK(is_preset.error() == RecipeOpError::IsPreset);
}

TEST_CASE("delete_version soft-deletes without touching other versions or presets") {
  auto db = Database::open_at(temp_db_path("recipe_delete"));
  ensure_default_presets(db);
  auto presets = list_presets(db);
  auto preset_id = presets[0].id;
  auto keep_id = create_version(db, preset_id, std::string("Keep"), VersionParams{}).value();
  auto gone_id = create_version(db, preset_id, std::string("Gone"), VersionParams{}).value();

  auto result = delete_version(db, gone_id);
  REQUIRE(result.ok());

  auto versions = list_versions(db, preset_id);
  REQUIRE(versions.size() == 2);  // 软删除:行还在，只是标记了
  auto gone = std::find_if(versions.begin(), versions.end(),
                            [&](const auto& v) { return v.id == gone_id; });
  REQUIRE(gone != versions.end());
  CHECK(gone->deleted);
  auto keep = std::find_if(versions.begin(), versions.end(),
                            [&](const auto& v) { return v.id == keep_id; });
  REQUIRE(keep != versions.end());
  CHECK_FALSE(keep->deleted);

  CHECK(list_presets(db).size() == presets.size());  // 预设本身不受影响
}

TEST_CASE("delete_version reports NotFound for missing/already-deleted, IsPreset for a preset") {
  auto db = Database::open_at(temp_db_path("recipe_delete_errors"));
  ensure_default_presets(db);
  auto preset_id = list_presets(db)[0].id;
  auto version_id = create_version(db, preset_id, std::nullopt, VersionParams{}).value();

  auto missing = delete_version(db, 999999);
  REQUIRE(!missing.ok());
  CHECK(missing.error() == RecipeOpError::NotFound);

  auto is_preset = delete_version(db, preset_id);
  REQUIRE(!is_preset.ok());
  CHECK(is_preset.error() == RecipeOpError::IsPreset);

  REQUIRE(delete_version(db, version_id).ok());
  auto again = delete_version(db, version_id);  // 已经软删除过，再删一次报错，不是幂等成功
  REQUIRE(!again.ok());
  CHECK(again.error() == RecipeOpError::NotFound);
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
