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

TEST_CASE("add_tag on an already-tagged image is idempotent even when the tag is at cap") {
  // 幂等检查在 cap 检查之前——已经打过的图片再选一次同一个标签,不管这个
  // 标签是否已经满,都应该直接幂等成功,不会触发 CapExceeded。这是
  // increment 6.4.4 的 space 菜单依赖的行为,之前"幂等"和"cap 超限"分开测
  // 过,没测过两者同时满足的组合。
  auto fx = make_fixture("idempotent_at_cap", 2);
  auto tag = create_tag(fx.db, fx.project_id, "精选", 1, false);
  REQUIRE(tag.ok());

  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());  // 打满 cap=1
  auto again = add_tag(fx.db, fx.images[0], tag.value());   // 同一张图再打一次
  REQUIRE(again.ok());
  CHECK(list_tags(fx.db, fx.project_id)[0].tagged_count == 1);  // 没有重复插入
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

TEST_CASE("tags_for_image returns only the tags actually applied to that image, sorted by name") {
  auto fx = make_fixture("tags_for_image", 2);
  auto selected = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  auto friends = create_tag(fx.db, fx.project_id, "朋友圈", std::nullopt, true);
  auto reject = create_tag(fx.db, fx.project_id, "废片", std::nullopt, false);
  REQUIRE(selected.ok());
  REQUIRE(friends.ok());
  REQUIRE(reject.ok());

  REQUIRE(add_tag(fx.db, fx.images[0], friends.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[0], selected.value()).ok());
  // images[1] 只打了"废片"，不应该出现在 images[0] 的结果里。
  REQUIRE(add_tag(fx.db, fx.images[1], reject.value()).ok());

  auto tags = tags_for_image(fx.db, fx.images[0]);
  REQUIRE(tags.size() == 2);
  CHECK(tags[0].name == "朋友圈");  // 按名字排序
  CHECK(tags[1].name == "精选");
}

TEST_CASE("tags_for_image returns an empty list for an untagged or unknown image") {
  auto fx = make_fixture("tags_for_image_empty", 1);
  CHECK(tags_for_image(fx.db, fx.images[0]).empty());
  CHECK(tags_for_image(fx.db, fx.images[0] + 999).empty());
}

TEST_CASE("delete_tag cascades image_tags rows for every image that had it") {
  auto fx = make_fixture("delete_tag_cascade", 2);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[1], tag.value()).ok());

  auto result = delete_tag(fx.db, tag.value());
  REQUIRE(result.ok());

  CHECK(tags_for_image(fx.db, fx.images[0]).empty());
  CHECK(tags_for_image(fx.db, fx.images[1]).empty());

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(fx.db.handle(), "SELECT COUNT(*) FROM image_tags WHERE tag_id = ?;", -1,
                      &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, tag.value());
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(stmt, 0) == 0);
  sqlite3_finalize(stmt);
}

TEST_CASE("delete_tag reports TagNotFound for a missing tag_id") {
  auto fx = make_fixture("delete_tag_missing", 1);
  auto result = delete_tag(fx.db, 999);
  REQUIRE(!result.ok());
  CHECK(result.error() == DeleteTagError::TagNotFound);
}

TEST_CASE("delete_tag is not idempotent - deleting an already-deleted tag_id errors") {
  auto fx = make_fixture("delete_tag_not_idempotent", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());

  REQUIRE(delete_tag(fx.db, tag.value()).ok());
  auto second = delete_tag(fx.db, tag.value());
  REQUIRE(!second.ok());
  CHECK(second.error() == DeleteTagError::TagNotFound);
}

TEST_CASE("delete_tag refuses a system tag and leaves it and its associations intact") {
  auto fx = make_fixture("delete_tag_system", 1);
  auto tag = create_tag(fx.db, fx.project_id, "废片", std::nullopt, false, /*is_system=*/true);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());

  auto result = delete_tag(fx.db, tag.value());
  REQUIRE(!result.ok());
  CHECK(result.error() == DeleteTagError::SystemTagProtected);

  auto tags = list_tags(fx.db, fx.project_id);
  REQUIRE(tags.size() == 1);
  CHECK(tags[0].id == tag.value());
  CHECK(tags[0].tagged_count == 1);
}

TEST_CASE("delete_tag only affects the targeted tag, not other tags on the same image") {
  auto fx = make_fixture("delete_tag_only_target", 1);
  auto keep = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  auto drop = create_tag(fx.db, fx.project_id, "朋友圈", std::nullopt, false);
  REQUIRE(keep.ok());
  REQUIRE(drop.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], keep.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[0], drop.value()).ok());

  REQUIRE(delete_tag(fx.db, drop.value()).ok());

  auto tags = tags_for_image(fx.db, fx.images[0]);
  REQUIRE(tags.size() == 1);
  CHECK(tags[0].id == keep.value());
}

TEST_CASE("ensure_reject_tag creates a conformant system tag when none exists") {
  auto fx = make_fixture("ensure_reject_fresh", 1);

  auto tag_id = ensure_reject_tag(fx.db, fx.project_id);

  auto tags = list_tags(fx.db, fx.project_id);
  REQUIRE(tags.size() == 1);
  CHECK(tags[0].id == tag_id);
  CHECK(tags[0].name == kRejectTagName);
  CHECK(tags[0].is_system);
  CHECK(!tags[0].cap.has_value());
  CHECK(!tags[0].is_ordered);
  CHECK(tags[0].tagged_count == 0);
}

TEST_CASE("ensure_reject_tag is a no-op when the tag already exists") {
  auto fx = make_fixture("ensure_reject_noop", 1);

  auto first = ensure_reject_tag(fx.db, fx.project_id);
  auto second = ensure_reject_tag(fx.db, fx.project_id);

  CHECK(first == second);
  CHECK(list_tags(fx.db, fx.project_id).size() == 1);
}

TEST_CASE("ensure_reject_tag is scoped to its own project") {
  auto fx = make_fixture("ensure_reject_isolation", 1);

  auto other_photos = fresh_photo_dir("ensure_reject_isolation_other_project");
  touch(other_photos / "other.jpg");
  auto other_project = create_project(fx.db, "other_proj", other_photos.string());
  REQUIRE(other_project.ok());

  auto tag_a = ensure_reject_tag(fx.db, fx.project_id);
  auto tag_b = ensure_reject_tag(fx.db, other_project.value());

  CHECK(tag_a != tag_b);
  CHECK(list_tags(fx.db, fx.project_id).size() == 1);
  CHECK(list_tags(fx.db, other_project.value()).size() == 1);
}

// M3：core/dedup 用的重复标记系统标签，见 docs/M3_Dedup_Eng_Design.md。
// 跟 ensure_reject_tag 是同一套逻辑，测试也照抄同一套。
TEST_CASE("ensure_duplicate_tag creates a conformant system tag when none exists") {
  auto fx = make_fixture("ensure_duplicate_fresh", 1);

  auto tag_id = ensure_duplicate_tag(fx.db, fx.project_id);

  auto tags = list_tags(fx.db, fx.project_id);
  REQUIRE(tags.size() == 1);
  CHECK(tags[0].id == tag_id);
  CHECK(tags[0].name == kDuplicateTagName);
  CHECK(tags[0].is_system);
  CHECK(!tags[0].cap.has_value());
  CHECK(!tags[0].is_ordered);
}

TEST_CASE("ensure_duplicate_tag is a no-op when the tag already exists") {
  auto fx = make_fixture("ensure_duplicate_noop", 1);

  auto first = ensure_duplicate_tag(fx.db, fx.project_id);
  auto second = ensure_duplicate_tag(fx.db, fx.project_id);

  CHECK(first == second);
  CHECK(list_tags(fx.db, fx.project_id).size() == 1);
}

TEST_CASE("ensure_reject_tag and ensure_duplicate_tag coexist as two distinct system tags") {
  auto fx = make_fixture("ensure_both_system_tags", 1);

  auto reject_id = ensure_reject_tag(fx.db, fx.project_id);
  auto duplicate_id = ensure_duplicate_tag(fx.db, fx.project_id);

  CHECK(reject_id != duplicate_id);
  CHECK(list_tags(fx.db, fx.project_id).size() == 2);
}

// F-26/F-09：只返回请求范围内、确实打了目标标签的图片，不是"这个标签下
// 所有图片"（那是 filter_by_tag 的职责）。
TEST_CASE("images_with_tag returns only the images in scope that carry the given tag") {
  auto fx = make_fixture("images_with_tag_basic", 3);
  auto reject = create_tag(fx.db, fx.project_id, "废片", std::nullopt, false);
  REQUIRE(reject.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], reject.value()).ok());
  // images[2] 也打了废片，但不在下面查询的 image_ids 范围内，不该出现。
  REQUIRE(add_tag(fx.db, fx.images[2], reject.value()).ok());

  auto result = images_with_tag(fx.db, {fx.images[0], fx.images[1]}, reject.value());
  CHECK(result.size() == 1);
  CHECK(result.count(fx.images[0]) == 1);
  CHECK(result.count(fx.images[1]) == 0);
  CHECK(result.count(fx.images[2]) == 0);

  CHECK(images_with_tag(fx.db, {}, reject.value()).empty());
}

// 500 是分块大小，不是硬上限——超过一个分块也要正确合并所有分块的结果。
TEST_CASE("images_with_tag correctly spans more than one 500-id chunk") {
  auto fx = make_fixture("images_with_tag_chunking", 1);
  auto reject = create_tag(fx.db, fx.project_id, "废片", std::nullopt, false);
  REQUIRE(reject.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], reject.value()).ok());

  std::vector<ImageId> ids;
  for (int i = 0; i < 600; ++i) ids.push_back(1000000 + i);  // 不存在的 id，凑数量
  ids.push_back(fx.images[0]);  // 唯一真的打了标签的 id，混在中间

  auto result = images_with_tag(fx.db, ids, reject.value());
  CHECK(result.size() == 1);
  CHECK(result.count(fx.images[0]) == 1);
}
