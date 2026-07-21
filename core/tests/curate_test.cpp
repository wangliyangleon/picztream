#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/curate/curate.h"
#include "core/db/database.h"
#include "core/decode/decode.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::encode_jpeg_file;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::tagging;
using namespace pzt::core::curate;

namespace {

std::string fresh_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / ("curate_" + tag + ".db")).string();
  fs::remove(path);
  return path;
}

fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / ("curate_" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << "x";
}

// 跟 core/tests/dedup_test.cpp 的 make_fixture 是同一个模式：建一个带 N
// 张图片(a.jpg, b.jpg, ...)的项目，返回 project_id、按文件名排序的
// image_id 列表、项目根目录。
struct Fixture {
  Database db;
  ProjectId project_id;
  std::vector<ImageId> images;  // images[0]="a.jpg", images[1]="b.jpg", ...
  std::string root_path;
};

Fixture make_fixture(const std::string& tag, int image_count) {
  auto db = Database::open_at(fresh_db_path(tag));
  auto photos = fresh_photo_dir(tag);
  for (int i = 0; i < image_count; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    touch(photos / (name + ".jpg"));
  }
  auto created = create_project(db, "proj", photos.string());
  REQUIRE(created.ok());

  std::vector<ImageId> images;
  for (int i = 0; i < image_count; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    auto id = find_image_by_path(db, created.value(), name + ".jpg");
    REQUIRE(id.has_value());
    images.push_back(*id);
  }
  return Fixture{std::move(db), created.value(), std::move(images), photos.string()};
}

void set_captured_at(Database& db, ImageId id, std::int64_t value) {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "UPDATE images SET captured_at = ? WHERE id = ?;", -1, &stmt,
                      nullptr);
  sqlite3_bind_int64(stmt, 1, value);
  sqlite3_bind_int64(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// 跟 dedup_test.cpp 的 write_solid_jpeg 同一个手法：两张字节完全相同的
// 纯色 JPEG 解码后逐像素相同，dHash 距离必为 0，不需要精确控制压缩细节
// 就能稳定制造"这是一组重复"的场景，供分簇相关用例覆盖真实解码路径。
bool write_solid_jpeg(const fs::path& path, int width, int height, unsigned char gray) {
  DecodedImage img;
  img.width = width;
  img.height = height;
  img.rgba.assign(static_cast<std::size_t>(width) * height * 4, gray);
  return encode_jpeg_file(img, path.string()).ok();
}

}  // namespace

// W2026-07-21：curate 不再看 evaluation 记录，纯标签排除。原来"未评估就
// 排除""未达标(gate)就排除"两条用例整合成这一条——未评估的图照样进候选。
TEST_CASE("curate includes unevaluated images (no evaluation dependency)") {
  auto fx = make_fixture("no_eval_included", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);  // 时间差够大，各自成簇

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10);
  CHECK(result.returned == 2);
  // 簇数(2) < count(5)：每簇一代表，按 captured_at 降序。
  CHECK(result.selected == std::vector<ImageId>{fx.images[1], fx.images[0]});
}

TEST_CASE("curate excludes reject-tagged and duplicate-tagged images") {
  auto fx = make_fixture("excluded_tags", 3);
  for (int i = 0; i < 3; ++i) {
    set_captured_at(fx.db, fx.images[i], 1000 + i * 10000);
  }
  auto reject_tag = ensure_reject_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[1], reject_tag).ok());
  auto dup_tag = ensure_duplicate_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[2], dup_tag).ok());

  auto result = curate(fx.db, fx.project_id, std::nullopt, 5, 20, 10);
  CHECK(result.selected == std::vector<ImageId>{fx.images[0]});
}

TEST_CASE("curate insufficient candidates returns all of them, not an error") {
  auto fx = make_fixture("insufficient", 1);
  set_captured_at(fx.db, fx.images[0], 1000);

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10);
  CHECK(result.requested == 5);
  CHECK(result.returned == 1);
}

TEST_CASE("curate picks one representative per cluster when clusters >= N") {
  // 纯色图片内部梯度恒为 0，compute_dhash 对任意灰度值都会算出同一个哈
  // 希(0)——真正决定分不分进同一簇的是 cluster_by_time 这一步的时间窗，
  // 不是灰度值本身(跟 dedup_test.cpp 的 facade 测试用例是同一个道理)。
  // a/b 时间挨得近(同一簇,keep=b 时间更新那张)，c/d 分别离 a/b、离彼此
  // 都超过 20 秒时间窗(各自成一簇)。count=2，簇数=3 >= 2。
  auto fx = make_fixture("clusters_ge_n", 4);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "d.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);    // 跟 a 差 5 秒,同簇 -> keep=b
  set_captured_at(fx.db, fx.images[2], 100000);  // 跟 a/b、跟 d 都差超过 20 秒,独立簇
  set_captured_at(fx.db, fx.images[3], 200000);  // 跟其它三张都差超过 20 秒,独立簇

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  REQUIRE(result.returned == 2);
  // W2026-07-21：去分数后是纯时间多样性。代表 = {b(簇{a,b}的 keep), c, d}，
  // a 因为跟 b 同簇被排除在代表之外，不会入选——多样性保护的核心断言。
  // farthest-point：seed 取最新 d(200000)，再选离 d 时间最远的 b(1005)。
  CHECK(result.selected == std::vector<ImageId>{fx.images[3], fx.images[1]});
}

TEST_CASE("curate spreads selection across captured_at (time diversity)") {
  // 3 张纯色图，两两时间差都超过 20 秒时间窗，各自独立成簇(纯色图内部
  // 梯度恒为 0，真正拆开它们的是时间窗，不是灰度值，见上一条用例)。
  auto fx = make_fixture("tie_break_spread", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 0);
  set_captured_at(fx.db, fx.images[1], 100000);  // 离 a 差 100000
  set_captured_at(fx.db, fx.images[2], 100);     // 离 a 差 100

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  // W2026-07-21：纯时间多样性。seed 取最新 b(100000)；第二名额 a vs c，
  // 选离已选集(b)时间更远的 -> a(差 100000 > c 的 99900)。
  CHECK(result.selected == std::vector<ImageId>{fx.images[1], fx.images[0]});
}

TEST_CASE("curate does not backfill from non-representative cluster members when clusters < N") {
  // 一簇 3 张近重复(a,b,c)，keep=c(captured_at 最新)，全部落进同一个候
  // 选池，count=2 > 簇数=1：只返回代表 c，不从簇内非代表回填凑数——回填
  // 会让结果里出现彼此近重复的图，违背多样性目的(见 curate.cpp 说明)。
  // returned(1) < requested(2)，不报错。
  auto fx = make_fixture("no_backfill", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  for (int i = 0; i < 3; ++i) set_captured_at(fx.db, fx.images[i], 1000 + i);  // c 最新 -> keep

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  CHECK(result.requested == 2);
  CHECK(result.returned == 1);
  CHECK(result.selected == std::vector<ImageId>{fx.images[2]});
}

TEST_CASE("curate returns one representative per cluster across multiple clusters when clusters < N") {
  // 两簇：{a,b}(keep=b,时间更新那张) 和 {c}(独立簇)，count=3 > 簇数=2。
  // 只返回两个代表，按 captured_at 降序 [c, b]，不回填 a。
  auto fx = make_fixture("multi_cluster_shortfall", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);    // 跟 a 同簇 -> keep=b
  set_captured_at(fx.db, fx.images[2], 100000);  // 独立簇

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);
  CHECK(result.returned == 2);
  CHECK(result.selected == std::vector<ImageId>{fx.images[2], fx.images[1]});
}

TEST_CASE("curate is deterministic across repeated calls with identical input") {
  auto fx = make_fixture("determinism", 5);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);
  set_captured_at(fx.db, fx.images[2], 9000);
  set_captured_at(fx.db, fx.images[3], 13000);
  set_captured_at(fx.db, fx.images[4], 17000);

  auto first = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);
  auto second = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);

  CHECK(first.requested == second.requested);
  CHECK(first.returned == second.returned);
  CHECK(first.selected == second.selected);
}
