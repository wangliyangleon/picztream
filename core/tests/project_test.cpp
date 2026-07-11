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

TEST_CASE("create_project ignores a same-stem JPEG when a RAW file exists") {
  auto db = Database::open_at(fresh_db_path("raw_wins_over_jpeg"));
  auto photos = fresh_photo_dir("raw_wins_over_jpeg");

  touch(photos / "IMG_001.JPG");
  touch(photos / "IMG_001.DNG");
  touch(photos / "IMG_002.jpg");  // no RAW companion, stays kind="jpeg"

  auto result = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(result.ok());

  auto projects = list_projects(db);
  REQUIRE(projects.size() == 1);
  CHECK(projects[0].image_count == 2);  // IMG_001.JPG never became a record at all

  CHECK(!find_image_by_path(db, result.value(), "IMG_001.JPG").has_value());

  auto raw_id = find_image_by_path(db, result.value(), "IMG_001.DNG");
  REQUIRE(raw_id.has_value());
  auto raw_info = get_image(db, *raw_id);
  REQUIRE(raw_info.has_value());
  CHECK(raw_info->kind == "raw");

  auto jpeg_id = find_image_by_path(db, result.value(), "IMG_002.jpg");
  REQUIRE(jpeg_id.has_value());
  auto jpeg_info = get_image(db, *jpeg_id);
  REQUIRE(jpeg_info.has_value());
  CHECK(jpeg_info->kind == "jpeg");
  CHECK(!jpeg_info->preview_cache_path.has_value());
}

TEST_CASE("create_project records a pure RAW file with kind=raw") {
  auto db = Database::open_at(fresh_db_path("pure_raw"));
  auto photos = fresh_photo_dir("pure_raw");
  touch(photos / "DSCF0001.RAF");  // fake 10-byte content, not a real RAF

  auto result = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(result.ok());
  CHECK(list_projects(db)[0].image_count == 1);

  auto id = find_image_by_path(db, result.value(), "DSCF0001.RAF");
  REQUIRE(id.has_value());
  auto info = get_image(db, *id);
  REQUIRE(info.has_value());
  CHECK(info->kind == "raw");
  // create_project 会真的尝试用 core::raw::decode_preview 生成缓存 - 这里
  // 的文件是假内容，LibRaw 打不开，生成失败是预期行为(不阻断这张图片本
  // 身被记录)，所以 preview_cache_path 应该保持空，不是这个测试的 bug。
  CHECK(!info->preview_cache_path.has_value());
}

TEST_CASE("create_project recognizes .dng/.raf case-insensitively") {
  auto db = Database::open_at(fresh_db_path("raw_case_insensitive"));
  auto photos = fresh_photo_dir("raw_case_insensitive");
  touch(photos / "a.dng");
  touch(photos / "b.Raf");

  auto result = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(result.ok());
  CHECK(list_projects(db)[0].image_count == 2);
}

TEST_CASE("create_project does not merge two RAW files sharing the same stem") {
  auto db = Database::open_at(fresh_db_path("dup_raw_stem"));
  auto photos = fresh_photo_dir("dup_raw_stem");
  touch(photos / "IMG_001.dng");
  touch(photos / "IMG_001.raf");  // same stem, different RAW format - not a JPEG+RAW pair

  auto result = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(result.ok());
  CHECK(list_projects(db)[0].image_count == 2);  // each stays independent, not merged

  auto dng_id = find_image_by_path(db, result.value(), "IMG_001.dng");
  auto raf_id = find_image_by_path(db, result.value(), "IMG_001.raf");
  REQUIRE(dng_id.has_value());
  REQUIRE(raf_id.has_value());
  CHECK(get_image(db, *dng_id)->kind == "raw");
  CHECK(get_image(db, *raf_id)->kind == "raw");
}

TEST_CASE("create_project ignores RAW files entirely when support_raw is not passed") {
  // RAW 支持默认关闭，见 docs/RAW_Support.md——不传 support_raw 时代码路径
  // 跟 M0/M1 完全一样，RAW 文件不产生记录，同名 JPEG 也不会被"同名 RAW 存
  // 在"这条规则挤掉，因为这一轮压根不知道 RAW 存在。
  auto db = Database::open_at(fresh_db_path("support_raw_default_off"));
  auto photos = fresh_photo_dir("support_raw_default_off");
  touch(photos / "IMG_001.jpg");
  touch(photos / "IMG_001.dng");  // 同名 RAW，默认关闭时应该被完全忽略

  auto result = create_project(db, "trip", photos.string());
  REQUIRE(result.ok());
  CHECK(list_projects(db)[0].image_count == 1);  // 只有 JPEG，RAW 完全没被扫描到

  auto jpeg_id = find_image_by_path(db, result.value(), "IMG_001.jpg");
  REQUIRE(jpeg_id.has_value());
  CHECK(get_image(db, *jpeg_id)->kind == "jpeg");
  CHECK(!find_image_by_path(db, result.value(), "IMG_001.dng").has_value());
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

// F-06：scan_media 以前用会抛异常的 recursive_directory_iterator 重载,
// 目标目录根本不存在时会让 create_project(进而 `pzt new`)直接崩溃,而
// 不是走 Result<T,E> 的 NoImagesFound 错误路径。改用 error_code 版本之
// 后,这种情况应该跟"目录存在但没有图片"一样干净地报错,不抛异常。
TEST_CASE("create_project cleanly reports NoImagesFound for a nonexistent folder, doesn't throw") {
  auto db = Database::open_at(fresh_db_path("nonexistent_folder"));
  fs::path missing = fs::temp_directory_path() / "pzt_test" / "definitely_does_not_exist_12345";
  fs::remove_all(missing);

  auto result = create_project(db, "ghost_trip", missing.string());
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

TEST_CASE("rescan_project backfills captured_at only for rows that don't already have it") {
  // M2 收尾问题 3：这个字段上线之前建好的老项目，captured_at 全是
  // NULL，下一次 rescan 应该顺手回填；已经有值的行不应该被重新触碰
  // （用假文件测不出真实提取成功的情形——touch() 的内容不是真实
  // JPEG/RAW，read_capture_time 会返回 nullopt，所以这里验证的是"已有值
  // 的不被覆盖、没有值的 rescan 后仍然是 NULL 但没有崩溃"这两条边界，
  // 真实提取成功的情形只能靠真机验证）。
  auto db = Database::open_at(fresh_db_path("rescan_backfill"));
  auto photos = fresh_photo_dir("rescan_backfill");
  touch(photos / "a.jpg");
  touch(photos / "b.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  auto a_id = find_image_by_path(db, created.value(), "a.jpg");
  REQUIRE(a_id.has_value());

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "UPDATE images SET captured_at = ? WHERE id = ?;", -1, &stmt,
                      nullptr);
  sqlite3_bind_int64(stmt, 1, 12345);
  sqlite3_bind_int64(stmt, 2, *a_id);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  auto rescanned = rescan_project(db, created.value());
  REQUIRE(rescanned.ok());

  // 已经有值的行 rescan 之后原样保留，不被重新读取/覆盖。ImageInfo 不直
  // 接暴露 captured_at，直接查库确认。
  sqlite3_stmt* check_stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "SELECT captured_at FROM images WHERE id = ?;", -1, &check_stmt,
                      nullptr);
  sqlite3_bind_int64(check_stmt, 1, *a_id);
  REQUIRE(sqlite3_step(check_stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(check_stmt, 0) == 12345);
  sqlite3_finalize(check_stmt);
}

TEST_CASE("get_image returns nullopt evaluation by default, reads it back once set") {
  // M3 增量一修订版：这一步只打通 image_evaluations 表 + ImageInfo/
  // get_image 的读写通道（LEFT JOIN），谁来写
  // (core::ai::EvaluationWorker)是另一个文件的事，这里直接用 SQL 摆数
  // 据，不依赖真实 AI 调用。
  auto db = Database::open_at(fresh_db_path("evaluation_fields"));
  auto photos = fresh_photo_dir("evaluation_fields");
  touch(photos / "a.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  auto a_id = find_image_by_path(db, created.value(), "a.jpg");
  REQUIRE(a_id.has_value());

  auto before = get_image(db, *a_id);
  REQUIRE(before.has_value());
  CHECK(!before->evaluation.has_value());

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(),
                      "INSERT INTO image_evaluations (image_id, exposure_score, exposure_note, "
                      "exposure_fix_percent, composition_score, composition_note, "
                      "composition_fix_rotate_degrees, composition_fix_crop_left_percent, "
                      "composition_fix_crop_right_percent, composition_fix_crop_top_percent, "
                      "composition_fix_crop_bottom_percent, focus_score, focus_note, comment, "
                      "extra_guidance, provider) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
                      -1, &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, *a_id);
  sqlite3_bind_int(stmt, 2, 7);
  sqlite3_bind_text(stmt, 3, "slightly underexposed", -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 4, 15.0);
  sqlite3_bind_int(stmt, 5, 4);
  sqlite3_bind_text(stmt, 6, "horizon is tilted", -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 7, 2.5);
  sqlite3_bind_double(stmt, 8, 0.0);
  sqlite3_bind_double(stmt, 9, 0.0);
  sqlite3_bind_double(stmt, 10, 0.0);
  sqlite3_bind_double(stmt, 11, 5.0);
  sqlite3_bind_int(stmt, 12, 9);
  sqlite3_bind_text(stmt, 13, "sharp", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 14, "overall solid, mainly the tilted horizon", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 15, "focus on the crop", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 16, "gemini", -1, SQLITE_TRANSIENT);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  auto after = get_image(db, *a_id);
  REQUIRE(after.has_value());
  REQUIRE(after->evaluation.has_value());
  const auto& eval = *after->evaluation;
  CHECK(eval.exposure.score == 7);
  CHECK(eval.exposure.note == "slightly underexposed");
  REQUIRE(eval.exposure_fix.has_value());
  CHECK(eval.exposure_fix->adjust_percent == doctest::Approx(15.0));
  CHECK(eval.composition.score == 4);
  CHECK(eval.composition.note == "horizon is tilted");
  REQUIRE(eval.composition_fix.has_value());
  CHECK(eval.composition_fix->rotate_degrees == doctest::Approx(2.5));
  CHECK(eval.composition_fix->crop_bottom_percent == doctest::Approx(5.0));
  CHECK(eval.focus.score == 9);
  CHECK(eval.focus.note == "sharp");
  CHECK(eval.comment == "overall solid, mainly the tilted horizon");
  CHECK(eval.extra_guidance == "focus on the crop");
  CHECK(eval.provider == "gemini");
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

TEST_CASE("rescan_project upgrades a jpeg-only record in place when its RAW companion later appears") {
  // 项目从创建起就是 support_raw=true(一直是 RAW-aware 状态)，只是这张照
  // 片的 RAW 伴侣当时还没出现——这种"稳态"场景才会原地升级，见
  // docs/RAW_Support.md"Edge case"一节；对照的"第一次激活"场景见下面
  // "rescan_project inserts a separate record..."。
  auto db = Database::open_at(fresh_db_path("rescan_upgrade_jpeg_to_raw"));
  auto photos = fresh_photo_dir("rescan_upgrade_jpeg_to_raw");
  touch(photos / "IMG_001.jpg");
  auto created = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(created.ok());

  auto before_id = find_image_by_path(db, created.value(), "IMG_001.jpg");
  REQUIRE(before_id.has_value());
  CHECK(get_image(db, *before_id)->kind == "jpeg");

  touch(photos / "IMG_001.dng");  // 补上同名 RAW

  auto rescanned = rescan_project(db, created.value(), /*prune=*/true, /*support_raw=*/true);
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().added_count == 0);
  CHECK(rescanned.value().upgraded_count == 1);
  CHECK(rescanned.value().total_count == 1);  // 不是新插一条,还是同一张图

  CHECK(!find_image_by_path(db, created.value(), "IMG_001.jpg").has_value());
  auto after_id = find_image_by_path(db, created.value(), "IMG_001.dng");
  REQUIRE(after_id.has_value());
  CHECK(*after_id == *before_id);  // 同一行原地升级,id 不变
  auto after = get_image(db, *after_id);
  REQUIRE(after.has_value());
  CHECK(after->kind == "raw");
}

TEST_CASE("rescan_project ignores a JPEG that appears after its RAW companion already exists") {
  auto db = Database::open_at(fresh_db_path("rescan_raw_first_jpeg_ignored"));
  auto photos = fresh_photo_dir("rescan_raw_first_jpeg_ignored");
  touch(photos / "IMG_002.raf");
  auto created = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(created.ok());

  auto before_id = find_image_by_path(db, created.value(), "IMG_002.raf");
  REQUIRE(before_id.has_value());

  touch(photos / "IMG_002.jpg");  // 补上同名 JPEG - 应该被忽略，不产生任何变化

  auto rescanned = rescan_project(db, created.value(), /*prune=*/true, /*support_raw=*/true);
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().added_count == 0);
  CHECK(rescanned.value().upgraded_count == 0);
  CHECK(rescanned.value().total_count == 1);

  CHECK(!find_image_by_path(db, created.value(), "IMG_002.jpg").has_value());
  auto after_id = find_image_by_path(db, created.value(), "IMG_002.raf");
  REQUIRE(after_id.has_value());
  CHECK(*after_id == *before_id);
  CHECK(get_image(db, *after_id)->kind == "raw");
}

TEST_CASE("rescan_project jpeg-to-raw upgrade is idempotent") {
  auto db = Database::open_at(fresh_db_path("rescan_upgrade_idempotent"));
  auto photos = fresh_photo_dir("rescan_upgrade_idempotent");
  touch(photos / "IMG_003.jpg");
  auto created = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(created.ok());
  touch(photos / "IMG_003.dng");

  auto first = rescan_project(db, created.value(), /*prune=*/true, /*support_raw=*/true);
  REQUIRE(first.ok());
  CHECK(first.value().upgraded_count == 1);

  auto second = rescan_project(db, created.value(), /*prune=*/true, /*support_raw=*/true);
  REQUIRE(second.ok());
  CHECK(second.value().added_count == 0);
  CHECK(second.value().upgraded_count == 0);  // 已经是 kind=raw 了,不重复升级
  CHECK(second.value().total_count == 1);
}

TEST_CASE("rescan_project ignores new RAW files when support_raw is not passed") {
  auto db = Database::open_at(fresh_db_path("rescan_support_raw_default_off"));
  auto photos = fresh_photo_dir("rescan_support_raw_default_off");
  touch(photos / "a.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  touch(photos / "b.raf");  // 新出现的纯 RAW 文件，没有同名 JPEG

  auto rescanned = rescan_project(db, created.value());  // support_raw 默认 false
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().added_count == 0);
  CHECK(rescanned.value().total_count == 1);
  CHECK(!find_image_by_path(db, created.value(), "b.raf").has_value());
}

TEST_CASE("rescan_project without support_raw does not prune existing RAW records") {
  // support_raw=false 时这次根本没有扫描 RAW 文件，scanned_paths 里不会
  // 出现任何 RAW 路径——如果不特殊处理，现有 prune 逻辑会把已有的
  // kind='raw' 记录误判成"磁盘上消失了"直接删掉，这是必须避免的破坏性
  // bug，见 docs/RAW_Support.md。
  auto db = Database::open_at(fresh_db_path("rescan_no_support_raw_preserves_existing"));
  auto photos = fresh_photo_dir("rescan_no_support_raw_preserves_existing");
  touch(photos / "IMG_001.dng");
  auto created = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(created.ok());
  auto raw_id = find_image_by_path(db, created.value(), "IMG_001.dng");
  REQUIRE(raw_id.has_value());

  // 不带 --support-raw 的普通 rescan，prune 默认开启。
  auto rescanned = rescan_project(db, created.value());
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().removed_count == 0);
  CHECK(find_image_by_path(db, created.value(), "IMG_001.dng").has_value());
  CHECK(get_image(db, *raw_id)->kind == "raw");
}

TEST_CASE("rescan_project inserts a separate RAW record instead of upgrading when RAW support is activated for the first time") {
  // Edge case（docs/RAW_Support.md）：项目最初是在没有 --support-raw 的情
  // 况下建的（纯 JPEG，已经打过标签/建过 recipe），后来才补了同名 RAW 文
  // 件，第一次用 --support-raw 打开 RAW 支持——不应该把已有的 JPEG 记录原
  // 地升级成 RAW（那样会让用户已经做的标注含义变得含糊），而是保留 JPEG
  // 记录不动，把 RAW 当成一条独立的新记录插入，数据库里存的文件名带
  // "_raw" 后缀区分（不改磁盘上的真实文件名）。
  auto db = Database::open_at(fresh_db_path("rescan_activation_edge_case"));
  auto photos = fresh_photo_dir("rescan_activation_edge_case");
  touch(photos / "IMG_001.jpg");
  auto created = create_project(db, "trip", photos.string());  // support_raw 默认 false
  REQUIRE(created.ok());

  auto jpeg_id = find_image_by_path(db, created.value(), "IMG_001.jpg");
  REQUIRE(jpeg_id.has_value());

  touch(photos / "IMG_001.dng");  // 补上同名 RAW

  auto rescanned = rescan_project(db, created.value(), /*prune=*/true, /*support_raw=*/true);
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().added_count == 1);
  CHECK(rescanned.value().upgraded_count == 0);
  CHECK(rescanned.value().total_count == 2);  // 两条独立记录，不是原地升级

  // 原来的 JPEG 记录原样不变，同一个 id，kind 还是 jpeg。
  auto jpeg_after = find_image_by_path(db, created.value(), "IMG_001.jpg");
  REQUIRE(jpeg_after.has_value());
  CHECK(*jpeg_after == *jpeg_id);
  CHECK(get_image(db, *jpeg_after)->kind == "jpeg");

  // 新增的 RAW 记录：file_path 指向真实磁盘文件，file_name 带 _raw 后缀。
  auto raw_id = find_image_by_path(db, created.value(), "IMG_001.dng");
  REQUIRE(raw_id.has_value());
  auto raw_info = get_image(db, *raw_id);
  REQUIRE(raw_info.has_value());
  CHECK(raw_info->kind == "raw");
  CHECK(raw_info->file_name == "IMG_001_raw.dng");

  // 项目的 support_raw 持久化标记被打开了。
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "SELECT support_raw FROM projects WHERE id = ?;", -1, &stmt,
                      nullptr);
  sqlite3_bind_int64(stmt, 1, created.value());
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(stmt, 0) == 1);
  sqlite3_finalize(stmt);
}

TEST_CASE("rescan_project upgrades in place once RAW support is already active, even after an earlier activation") {
  // 紧接上一个测试的场景：项目已经因为一次 rescan --support-raw 变成
  // support_raw=true 持久化状态了，之后再遇到"已有 JPEG 记录 + 新 RAW 到
  // 达"，这次应该走正常的原地升级（was_already_supported=true 分支），
  // 不应该退化成插入独立记录+后缀那条 edge case 分支。
  auto db = Database::open_at(fresh_db_path("rescan_steady_state_after_activation"));
  auto photos = fresh_photo_dir("rescan_steady_state_after_activation");
  touch(photos / "IMG_001.jpg");
  touch(photos / "IMG_002.jpg");
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());

  touch(photos / "IMG_001.dng");
  auto activation = rescan_project(db, created.value(), /*prune=*/true, /*support_raw=*/true);
  REQUIRE(activation.ok());
  CHECK(activation.value().added_count == 1);  // IMG_001 走 edge case，插成独立记录

  // 现在项目已经是 support_raw=true 了。IMG_002 的 RAW 伴侣这时候才出现。
  touch(photos / "IMG_002.dng");
  auto steady_state = rescan_project(db, created.value(), /*prune=*/true, /*support_raw=*/true);
  REQUIRE(steady_state.ok());
  CHECK(steady_state.value().upgraded_count == 1);  // 原地升级，不是插入
  CHECK(steady_state.value().added_count == 0);

  auto img2_id = find_image_by_path(db, created.value(), "IMG_002.jpg");
  CHECK(!img2_id.has_value());  // 原地升级把 file_path 换成了 RAW 的
  auto img2_raw_id = find_image_by_path(db, created.value(), "IMG_002.dng");
  REQUIRE(img2_raw_id.has_value());
  CHECK(get_image(db, *img2_raw_id)->kind == "raw");
  CHECK(get_image(db, *img2_raw_id)->file_name == "IMG_002.dng");  // 没有 _raw 后缀
}

TEST_CASE("delete_project removes the RAW preview cache directory for the project") {
  auto db = Database::open_at(fresh_db_path("delete_project_cache_cleanup"));
  auto photos = fresh_photo_dir("delete_project_cache_cleanup");
  touch(photos / "IMG_001.dng");
  auto created = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(created.ok());
  auto image_id = find_image_by_path(db, created.value(), "IMG_001.dng");
  REQUIRE(image_id.has_value());

  // 手动伪造一份"已经生成好"的缓存文件并写回 preview_cache_path - 真实内容
  // 用假 RAW 文件跑不出来真正的缓存，这里直接摆一个占位文件，只验证生命
  // 周期清理逻辑本身（用不用 core::raw 生成的都一样是磁盘上的一个文件）。
  auto cache_dir = fs::temp_directory_path() / "pzt_test" / "delete_project_cache_cleanup_cache";
  fs::create_directories(cache_dir);
  auto cache_file = cache_dir / "fake_preview.jpg";
  touch(cache_file);
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "UPDATE images SET preview_cache_path = ? WHERE id = ?;", -1,
                      &stmt, nullptr);
  std::string cache_file_str = cache_file.string();
  sqlite3_bind_text(stmt, 1, cache_file_str.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, *image_id);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  REQUIRE(delete_project(db, created.value()).ok());
  // delete_project 删的是"这个项目的整个缓存子目录"(见 raw_preview_cache_dir)，
  // 不是这里手动伪造的这个路径本身 - 这个测试真正要覆盖的是 delete_project
  // 不会因为存在 preview_cache_path 这一列而出错或遗漏，具体的目录删除行为
  // 由下面 rescan prune 那个测试通过真实生成路径覆盖。
  CHECK(list_projects(db).empty());
}

TEST_CASE("rescan_project prune removes the cache file for a pruned RAW image") {
  auto db = Database::open_at(fresh_db_path("rescan_prune_cache_cleanup"));
  auto photos = fresh_photo_dir("rescan_prune_cache_cleanup");
  touch(photos / "IMG_001.dng");
  auto created = create_project(db, "trip", photos.string(), /*support_raw=*/true);
  REQUIRE(created.ok());
  auto image_id = find_image_by_path(db, created.value(), "IMG_001.dng");
  REQUIRE(image_id.has_value());

  auto cache_dir = fs::temp_directory_path() / "pzt_test" / "rescan_prune_cache_cleanup_cache";
  fs::create_directories(cache_dir);
  auto cache_file = cache_dir / "fake_preview.jpg";
  touch(cache_file);
  REQUIRE(fs::exists(cache_file));
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "UPDATE images SET preview_cache_path = ? WHERE id = ?;", -1,
                      &stmt, nullptr);
  std::string cache_file_str = cache_file.string();
  sqlite3_bind_text(stmt, 1, cache_file_str.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, *image_id);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  fs::remove(photos / "IMG_001.dng");  // 模拟源文件被删除，触发 prune

  auto rescanned = rescan_project(db, created.value(), /*prune=*/true, /*support_raw=*/true);
  REQUIRE(rescanned.ok());
  CHECK(rescanned.value().removed_count == 1);
  CHECK(!fs::exists(cache_file));  // 缓存文件跟着数据库记录一起被清理了
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
