#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/decode/decode.h"
#include "core/export/export.h"
#include "core/project/project.h"
#include "core/recipe/recipe.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::encode_jpeg_file;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::exporting;
using namespace pzt::core::recipe;
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

// increment 7 的烘焙测试需要真正能被 decode_jpeg_file 解码的照片——上面
// 的 make_fixture 用 touch() 写的是 10 字节的假内容，扫描(只看扩展名)不
// 会失败，但解码会失败，现有测试从来不会走到解码这一步(从来没给图片应
// 用过 recipe)所以之前不受影响。这里用刚在 increment 4 提升成生产代码
// 的 encode_jpeg_file 现场生成纯色真实 JPEG，不用再重新抄一遍
// CGImageDestination 的写法。
void write_real_jpeg(const fs::path& path, int width, int height, std::uint8_t r, std::uint8_t g,
                      std::uint8_t b) {
  DecodedImage img;
  img.width = width;
  img.height = height;
  img.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i + 0] = r;
    img.rgba[i + 1] = g;
    img.rgba[i + 2] = b;
    img.rgba[i + 3] = 255;
  }
  fs::create_directories(path.parent_path());
  REQUIRE(encode_jpeg_file(img, path.string()).ok());
}

Fixture make_real_photo_fixture(const std::string& tag, int image_count) {
  auto db = Database::open_at(fresh_db_path(tag));
  ensure_default_presets(db);  // open_at 本身不播种，这是 api.cpp 门面才做的事
  auto photos = fresh_photo_dir(tag);
  for (int i = 0; i < image_count; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "img_%03d.jpg", i);
    write_real_jpeg(photos / name, 40, 30, 50, 100, 150);
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

std::vector<char> read_bytes(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
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
  CHECK(result.value().created_output_folder);  // fresh_output_dir 故意不预先创建
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
  CHECK_FALSE(result.value().created_output_folder);  // 目标文件夹这次是本来就有的
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
  CHECK(result.value().skipped[0].reason == SkipReason::SourceMissing);
  CHECK(fs::exists(out_dir / "img_001.jpg"));  // 另一张照常导出
}

// 应用了 recipe 的图片走烘焙路径(解码->渲染->编码),源文件存在但内容不是
// 合法 JPEG 时会在 decode_jpeg_file 这一步失败——跟"源文件缺失"是同一种
// "跳过这张、继续导出其它的"容错路径,只是失败的原因不同(SkipReason::
// DecodeFailed)。RenderFailed/EncodeFailed 这两种跳过原因在 export_tag
// 的实际管线里摸不到:images.recipe_id 的外键是 ON DELETE SET NULL(见
// core/db/schema.cpp),没法人为构造出一个指向不存在的行的 recipe_id 来触
// 发 RenderFailed;EncodeFailed 只在图片尺寸非正时触发,而这里的
// DecodedImage 全部来自"解码一张真实照片"，尺寸必为正，没有可行的构造
// 路径，所以不补这两种。
TEST_CASE("export_tag skips a recipe-applied image whose source content fails to decode") {
  auto db = Database::open_at(fresh_db_path("export_decode_fail"));
  ensure_default_presets(db);  // open_at 本身不播种，这是 api.cpp 门面才做的事
  auto photos = fresh_photo_dir("export_decode_fail");
  touch(photos / "img_000.jpg");  // 10 字节假内容，扫描能通过但解不出来
  auto created = create_project(db, "proj", photos.string());
  REQUIRE(created.ok());
  auto image_id = find_image_by_path(db, created.value(), "img_000.jpg");
  REQUIRE(image_id.has_value());

  auto warm_id = list_presets(db)[1].id;  // presets[0] 是 Origin，[1] 是 Warm
  REQUIRE(set_image_recipe(db, *image_id, warm_id).ok());

  auto tag = create_tag(db, created.value(), "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(db, *image_id, tag.value()).ok());

  auto out_dir = fresh_output_dir("export_decode_fail");
  auto result = export_tag(db, tag.value(), out_dir.string());
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 0);
  REQUIRE(result.value().skipped.size() == 1);
  CHECK(result.value().skipped[0].file_name == "img_000.jpg");
  CHECK(result.value().skipped[0].reason == SkipReason::DecodeFailed);
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

TEST_CASE("export_tag reports IoError when the output path can't be created as a directory") {
  auto fx = make_fixture("export_io_error", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());

  auto out_dir = fresh_output_dir("export_io_error");
  touch(out_dir);  // 路径上已经有个同名的普通文件，create_directories 建不了目录

  auto result = export_tag(fx.db, tag.value(), out_dir.string());
  REQUIRE(!result.ok());
  CHECK(result.error() == ExportTagError::IoError);
}

TEST_CASE("export_tag bakes a non-identity recipe into the exported image's bytes") {
  auto fx = make_real_photo_fixture("export_bake", 1);
  auto warm_id = list_presets(fx.db)[1].id;  // presets[0] 是 Origin(没有 LUT),[1] 是 Warm
  REQUIRE(set_image_recipe(fx.db, fx.images[0], warm_id).ok());

  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());

  auto out_dir = fresh_output_dir("export_bake");
  auto result = export_tag(fx.db, tag.value(), out_dir.string());
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 1);
  CHECK(result.value().skipped.empty());

  auto source_bytes = read_bytes(fx.photos_dir / "img_000.jpg");
  auto output_bytes = read_bytes(out_dir / "img_000.jpg");
  CHECK(source_bytes != output_bytes);  // 真的重新编码过,不是巧合的拷贝
}

TEST_CASE("export_tag leaves an image with no recipe byte-for-byte identical to the source") {
  auto fx = make_real_photo_fixture("export_no_recipe", 1);
  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());

  auto out_dir = fresh_output_dir("export_no_recipe");
  auto result = export_tag(fx.db, tag.value(), out_dir.string());
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 1);

  auto source_bytes = read_bytes(fx.photos_dir / "img_000.jpg");
  auto output_bytes = read_bytes(out_dir / "img_000.jpg");
  CHECK(source_bytes == output_bytes);  // 回归防线:没应用风格的图片必须字节级不变
}

TEST_CASE("export_tag --link degrades to a real file for a recipe-applied image "
          "but still symlinks the untouched one") {
  auto fx = make_real_photo_fixture("export_bake_link", 2);
  auto warm_id = list_presets(fx.db)[1].id;
  REQUIRE(set_image_recipe(fx.db, fx.images[0], warm_id).ok());
  // images[1] 不应用任何 recipe,保持对照。

  auto tag = create_tag(fx.db, fx.project_id, "精选", std::nullopt, false);
  REQUIRE(tag.ok());
  REQUIRE(add_tag(fx.db, fx.images[0], tag.value()).ok());
  REQUIRE(add_tag(fx.db, fx.images[1], tag.value()).ok());

  auto out_dir = fresh_output_dir("export_bake_link");
  auto result = export_tag(fx.db, tag.value(), out_dir.string(), LinkMode::Symlink);
  REQUIRE(result.ok());
  CHECK(result.value().exported_count == 2);

  // 应用了 recipe 的那张:输出是新生成的文件,没有"原始字节"可以软链,
  // link_mode 在这里被忽略。
  CHECK_FALSE(fs::is_symlink(out_dir / "img_000.jpg"));
  CHECK(fs::exists(out_dir / "img_000.jpg"));
  // 没应用 recipe 的那张:--link 语义不变,照常建符号链接。
  CHECK(fs::is_symlink(out_dir / "img_001.jpg"));
}
