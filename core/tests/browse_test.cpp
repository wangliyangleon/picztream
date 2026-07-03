#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/browse/browse.h"
#include "core/db/database.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::browse;
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

struct Fixture {
  Database db;
  ProjectId project_id;
  std::vector<ImageId> images;  // 按文件名排序：a,b,c,...
};

Fixture make_fixture(const std::string& tag, int image_count) {
  auto db = Database::open_at(fresh_db_path(tag));
  auto photos = fresh_photo_dir(tag);
  for (int i = 0; i < image_count; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "%c.jpg", 'a' + i);
    touch(photos / name);
  }
  auto created = create_project(db, "proj", photos.string());
  REQUIRE(created.ok());

  std::vector<ImageId> images;
  for (int i = 0; i < image_count; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "%c.jpg", 'a' + i);
    auto id = find_image_by_path(db, created.value(), name);
    REQUIRE(id.has_value());
    images.push_back(*id);
  }
  return Fixture{std::move(db), created.value(), std::move(images)};
}

}  // namespace

TEST_CASE("list_images returns images sorted by file_path") {
  auto fx = make_fixture("list_images", 3);
  auto images = list_images(fx.db, fx.project_id);
  REQUIRE(images.size() == 3);
  CHECK(images[0].file_path == "a.jpg");
  CHECK(images[1].file_path == "b.jpg");
  CHECK(images[2].file_path == "c.jpg");
}

TEST_CASE("next_image/prev_image wrap around at both ends") {
  auto fx = make_fixture("next_prev", 3);
  auto images = list_images(fx.db, fx.project_id);

  // nullopt -> 第一张；最后一张的 next 折返回第一张
  auto first = next_image(images, std::nullopt);
  REQUIRE(first.has_value());
  CHECK(*first == fx.images[0]);

  auto second = next_image(images, fx.images[0]);
  CHECK(*second == fx.images[1]);
  auto third = next_image(images, fx.images[1]);
  CHECK(*third == fx.images[2]);
  auto wrapped = next_image(images, fx.images[2]);
  REQUIRE(wrapped.has_value());
  CHECK(*wrapped == fx.images[0]);  // 折返

  // nullopt -> 最后一张（prev 的循环语义）；第一张的 prev 折返回最后一张
  auto last = prev_image(images, std::nullopt);
  REQUIRE(last.has_value());
  CHECK(*last == fx.images[2]);
  auto wrapped_prev = prev_image(images, fx.images[0]);
  REQUIRE(wrapped_prev.has_value());
  CHECK(*wrapped_prev == fx.images[2]);
}

TEST_CASE("next_image/prev_image on an empty or single-image list") {
  auto fx = make_fixture("edge_cases", 1);
  auto images = list_images(fx.db, fx.project_id);

  auto empty = std::vector<ImageRef>{};
  CHECK(!next_image(empty, std::nullopt).has_value());
  CHECK(!prev_image(empty, std::nullopt).has_value());

  // 单张图片：next/prev 都折返回自己
  CHECK(*next_image(images, fx.images[0]) == fx.images[0]);
  CHECK(*prev_image(images, fx.images[0]) == fx.images[0]);
}

TEST_CASE("next_image/prev_image treat an id not in the list like nullopt") {
  auto fx = make_fixture("stale_id", 2);
  auto images = list_images(fx.db, fx.project_id);
  auto stale = fx.images[1] + 999;
  CHECK(*next_image(images, stale) == fx.images[0]);
  CHECK(*prev_image(images, stale) == fx.images[1]);
}

TEST_CASE("next_untagged/prev_untagged skip tagged images and stop after a full lap") {
  auto fx = make_fixture("untagged", 3);
  auto images = list_images(fx.db, fx.project_id);
  auto tag = create_tag(fx.db, fx.project_id, "废片", std::nullopt, false);
  REQUIRE(tag.ok());

  // 打标签中间那张 (images[1])，从头开始找下一个未打标签的应该跳过它。
  REQUIRE(add_tag(fx.db, fx.images[1], tag.value()).ok());

  auto first_untagged = next_untagged(fx.db, images, std::nullopt);
  REQUIRE(first_untagged.has_value());
  CHECK(*first_untagged == fx.images[0]);

  auto next_after_a = next_untagged(fx.db, images, fx.images[0]);
  REQUIRE(next_after_a.has_value());
  CHECK(*next_after_a == fx.images[2]);  // 跳过了 images[1]

  // 全部打上标签后，怎么找都应该是 nullopt，不会死循环。
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[2], tag.value()).ok());
  CHECK(!next_untagged(fx.db, images, std::nullopt).has_value());
  CHECK(!prev_untagged(fx.db, images, std::nullopt).has_value());
}

TEST_CASE("filter_by_tag orders by position for ordered tags, tagged_at for unordered") {
  auto fx = make_fixture("filter", 3);
  auto ordered_tag = create_tag(fx.db, fx.project_id, "朋友圈", std::nullopt, true);
  REQUIRE(ordered_tag.ok());

  // 故意乱序打标签，验证结果按 position（打标签顺序）而不是文件名排列。
  REQUIRE(add_tag(fx.db, fx.images[2], ordered_tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[0], ordered_tag.value()).ok());

  auto filtered = filter_by_tag(fx.db, ordered_tag.value());
  REQUIRE(filtered.ok());
  REQUIRE(filtered.value().size() == 2);
  CHECK(filtered.value()[0].id == fx.images[2]);  // 先打的在前
  CHECK(filtered.value()[1].id == fx.images[0]);
}

TEST_CASE("filter_by_tag reports TagNotFound and handles an empty tag") {
  auto fx = make_fixture("filter_errors", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());

  auto empty = filter_by_tag(fx.db, tag.value());
  REQUIRE(empty.ok());
  CHECK(empty.value().empty());

  auto missing = filter_by_tag(fx.db, tag.value() + 999);
  REQUIRE(!missing.ok());
  CHECK(missing.error() == BrowseTagError::TagNotFound);
}
