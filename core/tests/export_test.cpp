#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/db/database.h"
#include "core/export/export.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::exporting;
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

fs::path fresh_output_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / (tag + "_out");
  fs::remove_all(dir);
  return dir;  // 故意不预先创建，测试 export_tag 自动建目录
}

void touch(const fs::path& p, std::size_t bytes = 10) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << std::string(bytes, 'x');
}

struct Fixture {
  Database db;
  ProjectId project_id;
  std::vector<ImageId> images;
  fs::path photos_dir;
};

Fixture make_fixture(const std::string& tag, int image_count) {
  auto db = Database::open_at(fresh_db_path(tag));
  auto photos = fresh_photo_dir(tag);
  for (int i = 0; i < image_count; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "img_%03d.jpg", i);
    touch(photos / name);
  }
  auto created = create_project(db, "proj", photos.string());
  REQUIRE(created.ok());

  std::vector<ImageId> images;
  for (int i = 0; i < image_count; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "img_%03d.jpg", i);
    auto id = find_image_by_path(db, created.value(), name);
    REQUIRE(id.has_value());
    images.push_back(*id);
  }
  return Fixture{std::move(db), created.value(), std::move(images), photos};
}

}  // namespace

TEST_CASE("export_tag on an unordered tag uses the original file name") {
  auto fx = make_fixture("export_unordered", 2);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());

  auto out_dir = fresh_output_dir("export_unordered");
  auto result = export_tag(fx.db, tag.value(), out_dir.string());
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 1);
  CHECK(result.value().skipped.empty());
  CHECK(fs::exists(out_dir / "img_000.jpg"));
}

TEST_CASE("export_tag on an ordered tag adds a zero-padded position prefix") {
  auto fx = make_fixture("export_ordered", 3);
  auto tag = create_tag(fx.db, fx.project_id, "朋友圈", std::nullopt, true);
  REQUIRE(tag.ok());
  // 故意乱序打标签，验证导出顺序跟着 position（打标签顺序）走。
  REQUIRE(add_tag(fx.db, fx.images[2], tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[1], tag.value()).ok());

  auto out_dir = fresh_output_dir("export_ordered");
  auto result = export_tag(fx.db, tag.value(), out_dir.string());
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 3);
  CHECK(fs::exists(out_dir / "01_img_002.jpg"));
  CHECK(fs::exists(out_dir / "02_img_000.jpg"));
  CHECK(fs::exists(out_dir / "03_img_001.jpg"));
}

TEST_CASE("export_tag widens the zero-padding when crossing the 100-item boundary") {
  auto fx = make_fixture("export_width", 100);
  auto tag = create_tag(fx.db, fx.project_id, "朋友圈", std::nullopt, true);
  REQUIRE(tag.ok());
  for (auto img : fx.images) {
    REQUIRE(add_tag(fx.db, img, tag.value()).ok());
  }

  auto out_dir = fresh_output_dir("export_width");
  auto result = export_tag(fx.db, tag.value(), out_dir.string());
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 100);
  CHECK(fs::exists(out_dir / "001_img_000.jpg"));  // 3 位宽
  CHECK(fs::exists(out_dir / "100_img_099.jpg"));
}

TEST_CASE("export_tag disambiguates a filename collision in the output folder") {
  auto fx = make_fixture("export_collision", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());

  auto out_dir = fresh_output_dir("export_collision");
  fs::create_directories(out_dir);
  touch(out_dir / "img_000.jpg");  // 目标文件夹里已经有同名文件

  auto result = export_tag(fx.db, tag.value(), out_dir.string());
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 1);
  CHECK(fs::exists(out_dir / "img_000.jpg"));    // 原来的文件没被覆盖
  CHECK(fs::exists(out_dir / "img_000_2.jpg"));  // 新导出的加了 _2
}

TEST_CASE("export_tag skips images whose source file is missing without aborting the rest") {
  auto fx = make_fixture("export_missing_source", 2);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[1], tag.value()).ok());

  fs::remove(fx.photos_dir / "img_000.jpg");  // 模拟源文件在磁盘上消失

  auto out_dir = fresh_output_dir("export_missing_source");
  auto result = export_tag(fx.db, tag.value(), out_dir.string());
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 1);
  REQUIRE(result.value().skipped.size() == 1);
  CHECK(result.value().skipped[0].file_name == "img_000.jpg");
  CHECK(result.value().skipped[0].reason == "源文件缺失");
  CHECK(fs::exists(out_dir / "img_001.jpg"));  // 另一张照常导出
}

TEST_CASE("export_tag with --link creates a symlink instead of copying bytes") {
  auto fx = make_fixture("export_link", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());

  auto out_dir = fresh_output_dir("export_link");
  auto result = export_tag(fx.db, tag.value(), out_dir.string(), LinkMode::Symlink);
  REQUIRE(result.ok());
  CHECK(fs::is_symlink(out_dir / "img_000.jpg"));
}

TEST_CASE("export_tag reports TagNotFound") {
  auto fx = make_fixture("export_errors", 1);
  auto out_dir = fresh_output_dir("export_errors");
  auto result = export_tag(fx.db, 999, out_dir.string());
  REQUIRE(!result.ok());
  CHECK(result.error() == ExportTagError::TagNotFound);
}
