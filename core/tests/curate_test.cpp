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

// 跟 dedup_test.cpp 的 insert_evaluation 同一套直接 SQL 摆数据手法，只关
// 心 overall_score()/passes_gate() 用到的三个分数。
void insert_evaluation(Database& db, ImageId id, int exposure_score, int composition_score,
                        int focus_score) {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(),
                      "INSERT INTO image_evaluations (image_id, exposure_score, exposure_note, "
                      "composition_score, composition_note, focus_score, focus_note, comment, "
                      "extra_guidance, provider) VALUES (?, ?, '', ?, '', ?, '', '', '', 'gemini');",
                      -1, &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_bind_int(stmt, 2, exposure_score);
  sqlite3_bind_int(stmt, 3, composition_score);
  sqlite3_bind_int(stmt, 4, focus_score);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
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

TEST_CASE("curate excludes images with no evaluation") {
  auto fx = make_fixture("no_eval", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);  // 时间差够大，不会同簇
  insert_evaluation(fx.db, fx.images[0], 8, 8, 8);
  // fx.images[1] 没评估

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10);
  CHECK(result.returned == 1);
  CHECK(result.selected == std::vector<ImageId>{fx.images[0]});
}

TEST_CASE("curate excludes images failing the evaluation gate") {
  auto fx = make_fixture("failing_gate", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);
  insert_evaluation(fx.db, fx.images[0], 8, 8, 8);
  insert_evaluation(fx.db, fx.images[1], 3, 8, 8);  // exposure 低于 gate

  auto result = curate(fx.db, fx.project_id, std::nullopt, 5, 20, 10);
  CHECK(result.selected == std::vector<ImageId>{fx.images[0]});
}

TEST_CASE("curate excludes reject-tagged and duplicate-tagged images") {
  auto fx = make_fixture("excluded_tags", 3);
  for (int i = 0; i < 3; ++i) {
    set_captured_at(fx.db, fx.images[i], 1000 + i * 10000);
    insert_evaluation(fx.db, fx.images[i], 8, 8, 8);
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
  insert_evaluation(fx.db, fx.images[0], 8, 8, 8);

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10);
  CHECK(result.requested == 5);
  CHECK(result.returned == 1);
}

TEST_CASE("curate picks one representative per cluster when clusters >= N") {
  // 纯色图片内部梯度恒为 0，compute_dhash 对任意灰度值都会算出同一个哈
  // 希(0)——真正决定分不分进同一簇的是 cluster_by_time 这一步的时间窗，
  // 不是灰度值本身(跟 dedup_test.cpp 的 facade 测试用例是同一个道理)。
  // a/b 时间挨得近(同一簇)，c/d 分别离 a/b、离彼此都超过 20 秒时间窗(各
  // 自成一簇)。count=2，簇数=3 >= 2。
  auto fx = make_fixture("clusters_ge_n", 4);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "d.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);    // 跟 a 差 5 秒,同簇
  set_captured_at(fx.db, fx.images[2], 100000);  // 跟 a/b、跟 d 都差超过 20 秒,独立簇
  set_captured_at(fx.db, fx.images[3], 200000);  // 跟其它三张都差超过 20 秒,独立簇
  insert_evaluation(fx.db, fx.images[0], 9, 9, 9);  // a：簇{a,b}代表(分数最高)
  insert_evaluation(fx.db, fx.images[1], 6, 6, 6);  // b：同簇，分数较低
  insert_evaluation(fx.db, fx.images[2], 8, 8, 8);  // c：独立簇
  insert_evaluation(fx.db, fx.images[3], 7, 7, 7);  // d：独立簇

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  REQUIRE(result.returned == 2);
  // 按 score 降序：a(簇{a,b}代表,9分) > c(8分) > d(7分)，b 因为跟 a 同簇
  // 被排除在代表之外，永远不会跟 a 同时入选——这是多样性保护的核心断言。
  CHECK(result.selected == std::vector<ImageId>{fx.images[0], fx.images[2]});
}

TEST_CASE("curate greedy tie-break spreads selection across captured_at when scores tie") {
  // 3 张纯色图，两两时间差都超过 20 秒时间窗，各自独立成簇(纯色图内部
  // 梯度恒为 0，真正拆开它们的是时间窗，不是灰度值，见上一条用例)；b/c
  // 同分，b 时间离已选(a)更远。
  auto fx = make_fixture("tie_break_spread", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 0);
  set_captured_at(fx.db, fx.images[1], 100000);  // 离 a 差 100000
  set_captured_at(fx.db, fx.images[2], 100);     // 离 a 差 100
  insert_evaluation(fx.db, fx.images[0], 9, 9, 9);  // 最高分，先选
  insert_evaluation(fx.db, fx.images[1], 7, 7, 7);  // 同分
  insert_evaluation(fx.db, fx.images[2], 7, 7, 7);  // 同分

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  // 第一个选 a(分数最高)；第二名额 b vs c 同分打平，按跟已选集(a, t=0)
  // 的时间差选更远的 -> b(差 100000 > 100)。
  CHECK(result.selected == std::vector<ImageId>{fx.images[0], fx.images[1]});
}

TEST_CASE("curate does not backfill from non-representative cluster members when clusters < N") {
  // 一簇 3 张(a 代表分最高，b/c 是它的近重复)，全部落进同一个候选池，
  // count=2 > 簇数=1：只返回代表 a，不从簇内非代表{b,c}里回填凑数——
  // 回填会让结果里出现彼此近重复的图，违背多样性目的(真机验证发现的
  // 问题，见 curate.cpp 里的说明)。returned(1) < requested(2)，不报错。
  auto fx = make_fixture("no_backfill", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  for (int i = 0; i < 3; ++i) set_captured_at(fx.db, fx.images[i], 1000 + i);
  insert_evaluation(fx.db, fx.images[0], 9, 9, 9);  // a: 代表
  insert_evaluation(fx.db, fx.images[1], 7, 7, 7);  // b
  insert_evaluation(fx.db, fx.images[2], 6, 6, 6);  // c

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  CHECK(result.requested == 2);
  CHECK(result.returned == 1);
  CHECK(result.selected == std::vector<ImageId>{fx.images[0]});
}

TEST_CASE("curate returns one representative per cluster across multiple clusters when clusters < N") {
  // 两簇：{a,b}(a 代表，分 9) 和 {c}(独立簇，分 7)，count=3 > 簇数=2。
  // 只返回两个代表，不回填 b。
  auto fx = make_fixture("multi_cluster_shortfall", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);    // 跟 a 同簇
  set_captured_at(fx.db, fx.images[2], 100000);  // 独立簇
  insert_evaluation(fx.db, fx.images[0], 9, 9, 9);
  insert_evaluation(fx.db, fx.images[1], 5, 5, 5);
  insert_evaluation(fx.db, fx.images[2], 7, 7, 7);

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);
  CHECK(result.returned == 2);
  CHECK(result.selected == std::vector<ImageId>{fx.images[0], fx.images[2]});
}

TEST_CASE("curate is deterministic across repeated calls with identical input") {
  auto fx = make_fixture("determinism", 5);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);
  set_captured_at(fx.db, fx.images[2], 9000);
  set_captured_at(fx.db, fx.images[3], 13000);
  set_captured_at(fx.db, fx.images[4], 17000);
  insert_evaluation(fx.db, fx.images[0], 8, 8, 8);
  insert_evaluation(fx.db, fx.images[1], 7, 7, 7);
  insert_evaluation(fx.db, fx.images[2], 9, 9, 9);
  insert_evaluation(fx.db, fx.images[3], 6, 6, 6);
  insert_evaluation(fx.db, fx.images[4], 8, 8, 8);

  auto first = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);
  auto second = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);

  CHECK(first.requested == second.requested);
  CHECK(first.returned == second.returned);
  CHECK(first.selected == second.selected);
}
