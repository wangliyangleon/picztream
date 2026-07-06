#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/db/database.h"
#include "core/db/schema.h"
#include "core/project/project.h"
#include "core/recipe/recipe.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::recipe;

namespace {

std::string temp_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / (tag + ".db")).string();
  fs::remove(path);
  return path;
}

// 照抄 export_test.cpp 的 Fixture 模式：一个真实项目 + 一张图片，供图片
// ↔ recipe 关联测试用（set_image_recipe 需要一个真实存在的 image_id）。
fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << "x";
}

struct ImageFixture {
  Database db;
  ImageId image_id;
};

ImageFixture make_image_fixture(const std::string& tag) {
  auto db = Database::open_at(temp_db_path(tag));
  ensure_default_presets(db);  // open_at 本身不播种，这是 api.cpp 门面才做的事
  auto photos = fresh_photo_dir(tag);
  touch(photos / "img_000.jpg");
  auto created = create_project(db, "proj", photos.string());
  REQUIRE(created.ok());
  auto image_id = find_image_by_path(db, created.value(), "img_000.jpg");
  REQUIRE(image_id.has_value());
  return ImageFixture{std::move(db), *image_id};
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

TEST_CASE("list_presets returns presets in creation order, Origin fixed at id 0") {
  auto db = Database::open_at(temp_db_path("recipe_order"));
  ensure_default_presets(db);

  auto presets = list_presets(db);
  REQUIRE(presets.size() == 2);
  CHECK(presets[0].id < presets[1].id);
  CHECK(presets[0].id == 0);
  CHECK(presets[0].name == "Origin");
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

TEST_CASE("set_image_recipe applies a version and get_image_recipe returns it") {
  auto fx = make_image_fixture("recipe_apply_version");
  auto preset_id = list_presets(fx.db)[0].id;
  auto version_id = create_version(fx.db, preset_id, std::string("Look"), VersionParams{}).value();

  auto result = set_image_recipe(fx.db, fx.image_id, version_id);
  REQUIRE(result.ok());
  CHECK(get_image_recipe(fx.db, fx.image_id) == version_id);
}

TEST_CASE("set_image_recipe applies a preset directly (its own neutral state)") {
  auto fx = make_image_fixture("recipe_apply_preset");
  auto preset_id = list_presets(fx.db)[0].id;

  REQUIRE(set_image_recipe(fx.db, fx.image_id, preset_id).ok());
  CHECK(get_image_recipe(fx.db, fx.image_id) == preset_id);
}

TEST_CASE("set_image_recipe with nullopt clears the association") {
  auto fx = make_image_fixture("recipe_apply_clear");
  auto preset_id = list_presets(fx.db)[0].id;
  REQUIRE(set_image_recipe(fx.db, fx.image_id, preset_id).ok());

  REQUIRE(set_image_recipe(fx.db, fx.image_id, std::nullopt).ok());
  CHECK_FALSE(get_image_recipe(fx.db, fx.image_id).has_value());
}

TEST_CASE("set_image_recipe reports ImageNotFound and RecipeNotFound") {
  auto fx = make_image_fixture("recipe_apply_errors");
  auto preset_id = list_presets(fx.db)[0].id;

  auto bad_image = set_image_recipe(fx.db, 999999, preset_id);
  REQUIRE(!bad_image.ok());
  CHECK(bad_image.error() == SetImageRecipeError::ImageNotFound);

  auto bad_recipe = set_image_recipe(fx.db, fx.image_id, 999999);
  REQUIRE(!bad_recipe.ok());
  CHECK(bad_recipe.error() == SetImageRecipeError::RecipeNotFound);

  auto version_id = create_version(fx.db, preset_id, std::nullopt, VersionParams{}).value();
  REQUIRE(delete_version(fx.db, version_id).ok());
  auto deleted_recipe = set_image_recipe(fx.db, fx.image_id, version_id);
  REQUIRE(!deleted_recipe.ok());
  CHECK(deleted_recipe.error() == SetImageRecipeError::RecipeNotFound);
}

TEST_CASE("get_image_recipe returns empty for a nonexistent image or one with no recipe applied") {
  auto fx = make_image_fixture("recipe_get_empty");
  CHECK_FALSE(get_image_recipe(fx.db, fx.image_id).has_value());  // 存在但没应用过
  CHECK_FALSE(get_image_recipe(fx.db, 999999).has_value());       // 不存在
}

TEST_CASE("image-recipe association persists across reopening the database") {
  std::string path = temp_db_path("recipe_persist");
  auto photos = fresh_photo_dir("recipe_persist");
  touch(photos / "img_000.jpg");

  ImageId image_id;
  RecipeId preset_id;
  {
    auto db = Database::open_at(path);
    ensure_default_presets(db);
    auto created = create_project(db, "proj", photos.string());
    REQUIRE(created.ok());
    image_id = *find_image_by_path(db, created.value(), "img_000.jpg");
    preset_id = list_presets(db)[0].id;
    REQUIRE(set_image_recipe(db, image_id, preset_id).ok());
  }
  {
    auto db = Database::open_at(path);  // 独立重开一个连接，模拟重启进程
    CHECK(get_image_recipe(db, image_id) == preset_id);
  }
}

TEST_CASE("describe_recipe returns structured preset/version names, not a pre-formatted string") {
  auto db = Database::open_at(temp_db_path("recipe_describe"));
  ensure_default_presets(db);
  auto preset_id = list_presets(db)[0].id;

  auto preset_desc = describe_recipe(db, preset_id);
  REQUIRE(preset_desc.has_value());
  CHECK(preset_desc->preset_name == "Origin");
  CHECK_FALSE(preset_desc->version_name.has_value());  // 直接应用预设本身,没有第二层

  auto named = create_version(db, preset_id, std::string("胶片感"), VersionParams{}).value();
  auto named_desc = describe_recipe(db, named);
  REQUIRE(named_desc.has_value());
  CHECK(named_desc->preset_name == "Origin");
  CHECK(named_desc->version_name == "胶片感");

  auto unnamed = create_version(db, preset_id, std::nullopt, VersionParams{}).value();
  auto unnamed_desc = describe_recipe(db, unnamed);
  REQUIRE(unnamed_desc.has_value());
  CHECK(unnamed_desc->version_name == "(未命名)");

  CHECK_FALSE(describe_recipe(db, 999999).has_value());
}

TEST_CASE("resolve_recipe resolves a LUT-bearing preset itself with all-zero params") {
  auto db = Database::open_at(temp_db_path("recipe_resolve_preset"));
  ensure_default_presets(db);
  auto warm_id = list_presets(db)[1].id;  // presets[0] 是 Origin，没有 LUT，专门有下面单独一条测试

  auto resolved = resolve_recipe(db, warm_id);
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->lut.has_value());
  CHECK(resolved->lut->size == 17);
  CHECK(resolved->params.highlights == 0);
  CHECK(resolved->params.shadows == 0);
  CHECK(resolved->params.wb_shift_r == 0);
  CHECK(resolved->params.wb_shift_b == 0);
}

TEST_CASE("resolve_recipe resolves a version using the parent preset's LUT plus its own params") {
  auto db = Database::open_at(temp_db_path("recipe_resolve_version"));
  ensure_default_presets(db);
  auto warm_id = list_presets(db)[1].id;
  auto version_id =
      create_version(db, warm_id, std::nullopt, VersionParams{10, -5, 3, -2}).value();

  auto resolved = resolve_recipe(db, version_id);
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->lut.has_value());  // 来自父预设，不是 version 自己(version 没有 base_lut)
  CHECK(resolved->lut->size == 17);
  CHECK(resolved->params.highlights == 10);
  CHECK(resolved->params.shadows == -5);
  CHECK(resolved->params.wb_shift_r == 3);
  CHECK(resolved->params.wb_shift_b == -2);
}

TEST_CASE("resolve_recipe returns no LUT for the Origin preset (skip apply_lut entirely)") {
  auto db = Database::open_at(temp_db_path("recipe_resolve_origin"));
  ensure_default_presets(db);
  auto origin_id = list_presets(db)[0].id;
  CHECK(origin_id == 0);

  auto resolved = resolve_recipe(db, origin_id);
  REQUIRE(resolved.has_value());
  CHECK_FALSE(resolved->lut.has_value());

  // version 挂在 Origin 下面同样没有 LUT，只有细节参数。
  auto version_id =
      create_version(db, origin_id, std::nullopt, VersionParams{5, 0, 0, 0}).value();
  auto version_resolved = resolve_recipe(db, version_id);
  REQUIRE(version_resolved.has_value());
  CHECK_FALSE(version_resolved->lut.has_value());
  CHECK(version_resolved->params.highlights == 5);
}

TEST_CASE("resolve_recipe still resolves an already soft-deleted version") {
  // 这是跟 set_image_recipe 刻意不同的地方：已经引用软删除 version 的图片
  // 必须继续能正常渲染，resolve_recipe/render 不能对软删除的东西报错。
  auto db = Database::open_at(temp_db_path("recipe_resolve_deleted"));
  ensure_default_presets(db);
  auto preset_id = list_presets(db)[0].id;
  auto version_id = create_version(db, preset_id, std::nullopt, VersionParams{5, 0, 0, 0}).value();
  REQUIRE(delete_version(db, version_id).ok());

  auto resolved = resolve_recipe(db, version_id);
  REQUIRE(resolved.has_value());
  CHECK(resolved->params.highlights == 5);
}

TEST_CASE("resolve_recipe returns empty for a nonexistent id") {
  auto db = Database::open_at(temp_db_path("recipe_resolve_missing"));
  ensure_default_presets(db);
  CHECK_FALSE(resolve_recipe(db, 999999).has_value());
}

TEST_CASE("render applies the resolved recipe to a copy, leaving the source untouched") {
  auto db = Database::open_at(temp_db_path("recipe_render"));
  ensure_default_presets(db);
  auto presets = list_presets(db);
  auto origin_id = presets[0].id;  // 没有 LUT,render 应该跳过 apply_lut,像素基本不变
  auto warm_id = presets[1].id;    // 非恒等的暖色偏移

  pzt::core::decode::DecodedImage src;
  src.width = 2;
  src.height = 1;
  src.rgba = {10, 200, 50, 255, 0, 0, 0, 255};
  auto src_copy = src.rgba;

  auto origin_result = render(db, src, origin_id);
  REQUIRE(origin_result.ok());
  for (std::size_t i = 0; i < src.rgba.size(); ++i) {
    CHECK(std::abs(static_cast<int>(origin_result.value().rgba[i]) -
                    static_cast<int>(src_copy[i])) <= 1);
  }
  CHECK(src.rgba == src_copy);  // render 不修改传入的原图

  auto warm_result = render(db, src, warm_id);
  REQUIRE(warm_result.ok());
  CHECK(warm_result.value().rgba != src_copy);  // 非恒等 LUT 确实改变了像素
}

TEST_CASE("render reports RecipeNotFound for a nonexistent id") {
  auto db = Database::open_at(temp_db_path("recipe_render_errors"));
  ensure_default_presets(db);
  pzt::core::decode::DecodedImage src;
  src.width = 1;
  src.height = 1;
  src.rgba = {0, 0, 0, 255};

  auto result = render(db, src, 999999);
  REQUIRE(!result.ok());
  CHECK(result.error() == RenderRecipeError::RecipeNotFound);
}
