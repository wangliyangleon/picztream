#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::tagging;

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

void touch(const fs::path& p, std::size_t bytes = 10) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << std::string(bytes, 'x');
}

// 建一个带 N 张图片(001.jpg..00N.jpg)的项目，返回 project_id 和按文件名排序
// 的 image_id 列表，方便测试用例直接按下标取图片。
struct Fixture {
  Database db;
  ProjectId project_id;
  std::vector<ImageId> images;
};

Fixture make_fixture(const std::string& tag, int image_count) {
  auto db = Database::open_at(fresh_db_path(tag));
  auto photos = fresh_photo_dir(tag);
  for (int i = 0; i < image_count; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "%03d.jpg", i);
    touch(photos / name);
  }
  auto created = create_project(db, "proj", photos.string());
  REQUIRE(created.ok());

  std::vector<ImageId> images;
  for (int i = 0; i < image_count; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "%03d.jpg", i);
    auto id = find_image_by_path(db, created.value(), name);
    REQUIRE(id.has_value());
    images.push_back(*id);
  }
  return Fixture{std::move(db), created.value(), std::move(images)};
}

}  // namespace

TEST_CASE("create_tag succeeds and rejects duplicate names") {
  auto fx = make_fixture("create_tag", 1);

  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());

  auto dup = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(!dup.ok());
  CHECK(dup.error() == CreateTagError::NameAlreadyExists);
}

TEST_CASE("add_tag on an unordered uncapped tag just inserts") {
  auto fx = make_fixture("add_unordered", 2);
  auto tag = create_tag(fx.db, fx.project_id, "废片", std::nullopt, false);
  REQUIRE(tag.ok());

  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  auto tags = list_tags(fx.db, fx.project_id);
  REQUIRE(tags.size() == 1);
  CHECK(tags[0].tagged_count == 1);
}

TEST_CASE("add_tag on an ordered tag assigns increasing positions") {
  auto fx = make_fixture("add_ordered", 3);
  auto tag = create_tag(fx.db, fx.project_id, "朋友圈", 9, true);
  REQUIRE(tag.ok());

  for (auto img : fx.images) {
    REQUIRE(add_tag(fx.db, img, tag.value()).ok());
  }

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(fx.db.handle(),
                      "SELECT position FROM image_tags WHERE tag_id = ? ORDER BY position ASC;",
                      -1, &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, tag.value());
  std::vector<std::int64_t> positions;
  while (sqlite3_step(stmt) == SQLITE_ROW) positions.push_back(sqlite3_column_int64(stmt, 0));
  sqlite3_finalize(stmt);

  REQUIRE(positions.size() == 3);
  CHECK(positions[0] == 1);
  CHECK(positions[1] == 2);
  CHECK(positions[2] == 3);
}

TEST_CASE("add_tag is idempotent - tagging the same image twice is a no-op success") {
  auto fx = make_fixture("add_idempotent", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());

  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());

  auto tags = list_tags(fx.db, fx.project_id);
  CHECK(tags[0].tagged_count == 1);  // 没有重复插入
}

TEST_CASE("add_tag reports CapExceeded with correctly ordered existing entries") {
  auto fx = make_fixture("cap_exceeded", 3);
  auto tag = create_tag(fx.db, fx.project_id, "朋友圈", 2, true);
  REQUIRE(tag.ok());

  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[1], tag.value()).ok());

  auto third = add_tag(fx.db, fx.images[2], tag.value());
  REQUIRE(!third.ok());
  CHECK(third.error().kind == AddTagFailureKind::CapExceeded);
  REQUIRE(third.error().cap_info.has_value());
  CHECK(third.error().cap_info->cap == 2);
  REQUIRE(third.error().cap_info->existing_entries.size() == 2);
  CHECK(third.error().cap_info->existing_entries[0].file_name == "000.jpg");
  CHECK(third.error().cap_info->existing_entries[1].file_name == "001.jpg");
}

TEST_CASE("add_tag reports TagNotFound/ImageNotFound/ProjectMismatch") {
  auto fx = make_fixture("add_errors", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());

  auto bad_tag = add_tag(fx.db, fx.images[0], tag.value() + 999);
  REQUIRE(!bad_tag.ok());
  CHECK(bad_tag.error().kind == AddTagFailureKind::TagNotFound);

  auto bad_image = add_tag(fx.db, fx.images[0] + 999, tag.value());
  REQUIRE(!bad_image.ok());
  CHECK(bad_image.error().kind == AddTagFailureKind::ImageNotFound);

  // 同一个数据库里另一个项目的图片打这个标签 -> ProjectMismatch。注意:必须
  // 是同一个 Database 里的第二个项目，不能用另建的一个全新数据库文件 - 两个
  // 独立数据库各自的 id 从 1 开始自增，跨库直接拿一个 id 去查会“碰巧”命中
  // 本库里同 id 的图片，测不出真正的跨项目校验。
  auto other_photos = fresh_photo_dir("add_errors_other_project");
  touch(other_photos / "other.jpg");
  auto other_project = create_project(fx.db, "other_proj", other_photos.string());
  REQUIRE(other_project.ok());
  auto other_image = find_image_by_path(fx.db, other_project.value(), "other.jpg");
  REQUIRE(other_image.has_value());

  auto mismatch = add_tag(fx.db, *other_image, tag.value());
  REQUIRE(!mismatch.ok());
  CHECK(mismatch.error().kind == AddTagFailureKind::ProjectMismatch);
}

TEST_CASE("remove_tag is idempotent and validates tag/image existence") {
  auto fx = make_fixture("remove_tag", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());

  // 本来就没打过标签，删除也算成功
  REQUIRE(remove_tag(fx.db, fx.images[0], tag.value()).ok());

  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  REQUIRE(remove_tag(fx.db, fx.images[0], tag.value()).ok());
  CHECK(list_tags(fx.db, fx.project_id)[0].tagged_count == 0);

  // 再删一次同样是幂等成功
  REQUIRE(remove_tag(fx.db, fx.images[0], tag.value()).ok());

  auto bad_tag = remove_tag(fx.db, fx.images[0], tag.value() + 999);
  REQUIRE(!bad_tag.ok());
  CHECK(bad_tag.error() == RemoveTagError::TagNotFound);

  auto bad_image = remove_tag(fx.db, fx.images[0] + 999, tag.value());
  REQUIRE(!bad_image.ok());
  CHECK(bad_image.error() == RemoveTagError::ImageNotFound);
}

TEST_CASE("replace_tag_entry preserves the original position") {
  auto fx = make_fixture("replace_position", 3);
  auto tag = create_tag(fx.db, fx.project_id, "朋友圈", 2, true);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());  // position 1
  REQUIRE(add_tag(fx.db, fx.images[1], tag.value()).ok());  // position 2

  REQUIRE(replace_tag_entry(fx.db, tag.value(), fx.images[0], fx.images[2]).ok());

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(fx.db.handle(), "SELECT position FROM image_tags WHERE tag_id = ? AND image_id = ?;",
                      -1, &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, tag.value());
  sqlite3_bind_int64(stmt, 2, fx.images[2]);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(stmt, 0) == 1);  // 接管了 images[0] 原来的 position
  sqlite3_finalize(stmt);

  CHECK(list_tags(fx.db, fx.project_id)[0].tagged_count == 2);  // 数量不变
}

TEST_CASE("replace_tag_entry reports OldImageNotTagged / NewImageNotFound") {
  auto fx = make_fixture("replace_errors", 2);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());

  auto not_tagged = replace_tag_entry(fx.db, tag.value(), fx.images[0], fx.images[1]);
  REQUIRE(!not_tagged.ok());
  CHECK(not_tagged.error() == ReplaceTagError::OldImageNotTagged);

  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  auto bad_new = replace_tag_entry(fx.db, tag.value(), fx.images[0], fx.images[1] + 999);
  REQUIRE(!bad_new.ok());
  CHECK(bad_new.error() == ReplaceTagError::NewImageNotFound);
}
