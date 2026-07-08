#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/db/database.h"
#include "core/project/project.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using pzt::core::project::archive_project;
using pzt::core::project::create_project;
using pzt::core::project::CreateProjectError;
using pzt::core::project::delete_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::find_project_by_name;
using pzt::core::project::find_project_by_root_path;
using pzt::core::project::get_image;
using pzt::core::project::list_projects;
using pzt::core::project::open_project;
using pzt::core::project::ProjectNotFoundError;
using pzt::core::project::rescan_project;

namespace {

std::string fresh_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / (tag + ".db")).string();
  fs::remove(path);
  return path;
}

fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

// Unit tests only care about extension/size, not real JPEG bytes - decoding
// is a later increment's concern.
void touch(const fs::path& p, std::size_t bytes = 10) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << std::string(bytes, 'x');
}

}  // namespace

TEST_CASE("create_project scans JPEGs recursively across subfolders") {
  auto db = Database::open_at(fresh_db_path("recursive_scan"));
  auto photos = fresh_photo_dir("recursive_scan");

  touch(photos / "a.jpg");
  touch(photos / "DCIM" / "100CANON" / "b.JPG");
  touch(photos / "DCIM" / "100CANON" / "sub" / "c.jpeg");
  touch(photos / "not_a_photo.txt");

  auto result = create_project(db, "trip", photos.string());
  REQUIRE(result.ok());

  auto projects = list_projects(db);
  REQUIRE(projects.size() == 1);
  CHECK(projects[0].name == "trip");
  CHECK(projects[0].image_count == 3);
}

TEST_CASE("create_project pairs a same-stem RAW+JPEG file into one raw_jpeg record") {
  auto db = Database::open_at(fresh_db_path("pair_raw_jpeg"));
  auto photos = fresh_photo_dir("pair_raw_jpeg");

  touch(photos / "IMG_001.JPG");
  touch(photos / "IMG_001.DNG");
  touch(photos / "IMG_002.jpg");  // unpaired JPEG, should stay kind="jpeg"

  auto result = create_project(db, "trip", photos.string());
  REQUIRE(result.ok());

  auto projects = list_projects(db);
  REQUIRE(projects.size() == 1);
  CHECK(projects[0].image_count == 2);  // paired file counts once, not twice

  auto paired_id = find_image_by_path(db, result.value(), "IMG_001.JPG");
  REQUIRE(paired_id.has_value());
  auto paired = get_image(db, *paired_id);
  REQUIRE(paired.has_value());
  CHECK(paired->kind == "raw_jpeg");
  REQUIRE(paired->raw_path.has_value());
  CHECK(*paired->raw_path == "IMG_001.DNG");

  auto unpaired_id = find_image_by_path(db, result.value(), "IMG_002.jpg");
  REQUIRE(unpaired_id.has_value());
  auto unpaired = get_image(db, *unpaired_id);
  REQUIRE(unpaired.has_value());
  CHECK(unpaired->kind == "jpeg");
  CHECK(!unpaired->raw_path.has_value());
}

TEST_CASE("create_project records a pure RAW file with kind=raw") {
  auto db = Database::open_at(fresh_db_path("pure_raw"));
  auto photos = fresh_photo_dir("pure_raw");
  touch(photos / "DSCF0001.RAF");

  auto result = create_project(db, "trip", photos.string());
  REQUIRE(result.ok());
  CHECK(list_projects(db)[0].image_count == 1);

  auto id = find_image_by_path(db, result.value(), "DSCF0001.RAF");
  REQUIRE(id.has_value());
  auto info = get_image(db, *id);
  REQUIRE(info.has_value());
  CHECK(info->kind == "raw");
  REQUIRE(info->raw_path.has_value());
  CHECK(*info->raw_path == "DSCF0001.RAF");
}

TEST_CASE("create_project recognizes .dng/.raf case-insensitively") {
  auto db = Database::open_at(fresh_db_path("raw_case_insensitive"));
  auto photos = fresh_photo_dir("raw_case_insensitive");
  touch(photos / "a.dng");
  touch(photos / "b.Raf");

  auto result = create_project(db, "trip", photos.string());
  REQUIRE(result.ok());
  CHECK(list_projects(db)[0].image_count == 2);
}

TEST_CASE("create_project does not pair two RAW files sharing the same stem") {
  auto db = Database::open_at(fresh_db_path("dup_raw_stem"));
  auto photos = fresh_photo_dir("dup_raw_stem");
  touch(photos / "IMG_001.dng");
  touch(photos / "IMG_001.raf");  // same stem, different RAW format - not a JPEG+RAW pair

  auto result = create_project(db, "trip", photos.string());
  REQUIRE(result.ok());
  CHECK(list_projects(db)[0].image_count == 2);  // each stays independent, not merged

  auto dng_id = find_image_by_path(db, result.value(), "IMG_001.dng");
  auto raf_id = find_image_by_path(db, result.value(), "IMG_001.raf");
  REQUIRE(dng_id.has_value());
  REQUIRE(raf_id.has_value());
  CHECK(get_image(db, *dng_id)->kind == "raw");
  CHECK(get_image(db, *raf_id)->kind == "raw");
}

TEST_CASE("create_project rejects an empty (no-JPEG) folder") {
  auto db = Database::open_at(fresh_db_path("no_jpegs"));
  auto photos = fresh_photo_dir("no_jpegs");
  touch(photos / "readme.txt");

  auto result = create_project(db, "empty_trip", photos.string());
  REQUIRE(!result.ok());
  CHECK(result.error() == CreateProjectError::NoImagesFound);
  CHECK(list_projects(db).empty());
}

TEST_CASE("create_project rejects a duplicate project name") {
  auto db = Database::open_at(fresh_db_path("dup_name"));
  auto photos = fresh_photo_dir("dup_name");
  touch(photos / "a.jpg");

  REQUIRE(create_project(db, "trip", photos.string()).ok());

  auto second = create_project(db, "trip", photos.string());
  REQUIRE(!second.ok());
  CHECK(second.error() == CreateProjectError::NameAlreadyExists);
}

TEST_CASE("list_projects sorts archived projects last") {
  auto db = Database::open_at(fresh_db_path("archived_sort"));
  auto photos_a = fresh_photo_dir("archived_sort_a");
  auto photos_b = fresh_photo_dir("archived_sort_b");
  touch(photos_a / "a.jpg");
  touch(photos_b / "b.jpg");

  REQUIRE(create_project(db, "zzz_active", photos_a.string()).ok());
  REQUIRE(create_project(db, "aaa_archived", photos_b.string()).ok());

  // archive_project() has its own dedicated test below - this one exercises
  // the ORDER BY logic directly against the schema in isolation.
  char* err = nullptr;
  sqlite3_exec(db.handle(), "UPDATE projects SET archived_at = 1 WHERE name = 'aaa_archived';",
               nullptr, nullptr, &err);
  REQUIRE(err == nullptr);

  auto projects = list_projects(db);
  REQUIRE(projects.size() == 2);
  CHECK(projects[0].name == "zzz_active");
  CHECK(!projects[0].archived);
  CHECK(projects[1].name == "aaa_archived");
  CHECK(projects[1].archived);
}

TEST_CASE("find_project_by_name hits and misses") {
  auto db = Database::open_at(fresh_db_path("find_by_name"));
  auto photos = fresh_photo_dir("find_by_name");
  touch(photos / "a.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  auto found = find_project_by_name(db, "trip");
  REQUIRE(found.has_value());
  CHECK(*found == created.value());

  CHECK(!find_project_by_name(db, "does_not_exist").has_value());
}

TEST_CASE("find_project_by_root_path hits and misses") {
  auto db = Database::open_at(fresh_db_path("find_by_path"));
  auto photos = fresh_photo_dir("find_by_path");
  touch(photos / "a.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  auto found = find_project_by_root_path(db, photos.string());
  REQUIRE(found.has_value());
  CHECK(*found == created.value());

  CHECK(!find_project_by_root_path(db, "/nonexistent/path").has_value());
}

TEST_CASE("open_project returns the project summary or NotFound") {
  auto db = Database::open_at(fresh_db_path("open_project"));
  auto photos = fresh_photo_dir("open_project");
  touch(photos / "a.jpg");
  touch(photos / "b.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  auto opened = open_project(db, created.value());
  REQUIRE(opened.ok());
  CHECK(opened.value().name == "trip");
  CHECK(opened.value().image_count == 2);
  CHECK(!opened.value().archived);

  auto missing = open_project(db, created.value() + 999);
  REQUIRE(!missing.ok());
  CHECK(missing.error() == ProjectNotFoundError::NotFound);
}

TEST_CASE("archive_project sets archived_at and is idempotent") {
  auto db = Database::open_at(fresh_db_path("archive_project"));
  auto photos = fresh_photo_dir("archive_project");
  touch(photos / "a.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  REQUIRE(archive_project(db, created.value()).ok());
  CHECK(open_project(db, created.value()).value().archived);

  // Archiving an already-archived project is a no-op success, not an error.
  REQUIRE(archive_project(db, created.value()).ok());

  auto missing = archive_project(db, created.value() + 999);
  REQUIRE(!missing.ok());
  CHECK(missing.error() == ProjectNotFoundError::NotFound);
}

TEST_CASE("delete_project cascades images and reports NotFound on a missing id") {
  auto db = Database::open_at(fresh_db_path("delete_project"));
  auto photos = fresh_photo_dir("delete_project");
  touch(photos / "a.jpg");
  touch(photos / "b.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  REQUIRE(delete_project(db, created.value()).ok());

  CHECK(list_projects(db).empty());

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "SELECT COUNT(*) FROM images WHERE project_id = ?;", -1, &stmt,
                      nullptr);
  sqlite3_bind_int64(stmt, 1, created.value());
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(stmt, 0) == 0);
  sqlite3_finalize(stmt);

  auto missing = delete_project(db, created.value());
  REQUIRE(!missing.ok());
  CHECK(missing.error() == ProjectNotFoundError::NotFound);
}

TEST_CASE("rescan_project only adds files missing from images, leaves existing rows alone") {
  auto db = Database::open_at(fresh_db_path("rescan"));
  auto photos = fresh_photo_dir("rescan");
  touch(photos / "a.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  // 建好之后往文件夹里再丢两张 - 模拟"项目建好之后又补了几张照片"这个场景。
  touch(photos / "b.jpg");
  touch(photos / "sub" / "c.jpg");

  auto rescanned = rescan_project(db, created.value());
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().added_count == 2);
  CHECK(rescanned.value().total_count == 3);

  auto projects = list_projects(db);
  REQUIRE(projects.size() == 1);
  CHECK(projects[0].image_count == 3);
}

TEST_CASE("rescan_project is idempotent - rescanning again with no new files adds nothing") {
  auto db = Database::open_at(fresh_db_path("rescan_idempotent"));
  auto photos = fresh_photo_dir("rescan_idempotent");
  touch(photos / "a.jpg");
  touch(photos / "b.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  auto first = rescan_project(db, created.value());
  REQUIRE(first.ok());
  CHECK(first.value().added_count == 0);
  CHECK(first.value().total_count == 2);

  auto second = rescan_project(db, created.value());
  REQUIRE(second.ok());
  CHECK(second.value().added_count == 0);
  CHECK(second.value().total_count == 2);
}

TEST_CASE("rescan_project reports NotFound for a missing project id") {
  auto db = Database::open_at(fresh_db_path("rescan_missing"));
  auto missing = rescan_project(db, 999);
  REQUIRE(!missing.ok());
  CHECK(missing.error() == ProjectNotFoundError::NotFound);
}

TEST_CASE("rescan_project prunes files missing from disk by default, cascading their tags") {
  auto db = Database::open_at(fresh_db_path("rescan_prune"));
  auto photos = fresh_photo_dir("rescan_prune");
  touch(photos / "a.jpg");
  touch(photos / "b.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  auto b_id = find_image_by_path(db, created.value(), "b.jpg");
  REQUIRE(b_id.has_value());

  // 给 b.jpg 打一个标签,直接写 SQL,不引入 core/tagging 依赖。
  sqlite3_stmt* insert_tag = nullptr;
  sqlite3_prepare_v2(db.handle(),
                      "INSERT INTO tags (project_id, name, is_ordered, is_system) "
                      "VALUES (?, '精选', 0, 0);",
                      -1, &insert_tag, nullptr);
  sqlite3_bind_int64(insert_tag, 1, created.value());
  REQUIRE(sqlite3_step(insert_tag) == SQLITE_DONE);
  sqlite3_finalize(insert_tag);
  std::int64_t tag_id = sqlite3_last_insert_rowid(db.handle());

  sqlite3_stmt* insert_image_tag = nullptr;
  sqlite3_prepare_v2(db.handle(),
                      "INSERT INTO image_tags (image_id, tag_id, tagged_at) VALUES (?, ?, 0);",
                      -1, &insert_image_tag, nullptr);
  sqlite3_bind_int64(insert_image_tag, 1, *b_id);
  sqlite3_bind_int64(insert_image_tag, 2, tag_id);
  REQUIRE(sqlite3_step(insert_image_tag) == SQLITE_DONE);
  sqlite3_finalize(insert_image_tag);

  fs::remove(photos / "b.jpg");  // 模拟用户手动删掉了这张照片
  touch(photos / "c.jpg");       // 顺便加一张新的,验证增删同一次 rescan 里都生效

  auto rescanned = rescan_project(db, created.value());
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().added_count == 1);
  CHECK(rescanned.value().removed_count == 1);
  CHECK(rescanned.value().total_count == 2);

  CHECK(!find_image_by_path(db, created.value(), "b.jpg").has_value());

  sqlite3_stmt* count_stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "SELECT COUNT(*) FROM image_tags WHERE tag_id = ?;", -1,
                      &count_stmt, nullptr);
  sqlite3_bind_int64(count_stmt, 1, tag_id);
  REQUIRE(sqlite3_step(count_stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(count_stmt, 0) == 0);  // 级联清掉了
  sqlite3_finalize(count_stmt);
}

TEST_CASE("rescan_project with prune=false preserves the old add-only behavior") {
  auto db = Database::open_at(fresh_db_path("rescan_no_prune"));
  auto photos = fresh_photo_dir("rescan_no_prune");
  touch(photos / "a.jpg");
  touch(photos / "b.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  fs::remove(photos / "b.jpg");
  touch(photos / "c.jpg");

  auto rescanned = rescan_project(db, created.value(), /*prune=*/false);
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().added_count == 1);
  CHECK(rescanned.value().removed_count == 0);
  CHECK(rescanned.value().total_count == 3);
  CHECK(find_image_by_path(db, created.value(), "b.jpg").has_value());
}
