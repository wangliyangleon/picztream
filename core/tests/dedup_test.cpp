#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "core/dedup/dedup.h"
#include "core/db/database.h"
#include "core/project/project.h"

namespace fs = std::filesystem;
using pzt::core::Result;
using pzt::core::db::Database;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::DecodeError;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::dedup;

namespace {

std::string fresh_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / ("dedup_" + tag + ".db")).string();
  fs::remove(path);
  return path;
}

fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / ("dedup_" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << "x";
}

// 跟 core/tests/tagging_test.cpp 的 make_fixture 是同一个模式：建一个带
// N 张图片(a.jpg, b.jpg, ...)的项目，返回 project_id、按文件名排序的
// image_id 列表、项目根目录(find_duplicates 的 root_path 参数要用)。
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

void mark_raw_with_cache(Database& db, ImageId id, const std::string& cache_path) {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "UPDATE images SET kind = 'raw', preview_cache_path = ? WHERE id = ?;",
                      -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, cache_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// 只关心 overall_score() 用到的三个分数，note/comment/extra_guidance/
// provider 这些 NOT NULL 字段填占位值——跟
// core/tests/project_test.cpp"get_image returns nullopt evaluation..."
// 用的是同一套直接 SQL 摆数据手法。
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

void set_luminance(DecodedImage& img, int x, int y, int value) {
  std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) +
                      static_cast<std::size_t>(x)) *
                     4;
  img.rgba[idx + 0] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 1] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 2] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 3] = 255;
}

// 反向构造一张 9x8 的合成图片，使 compute_dhash(...) 精确等于
// target_hash——每行从 128 起步，按目标 bit 决定往上还是往下走 5，8 步之
// 后仍然落在 [88,168] 内，不会越界。resize_rgba 对 9x8 目标尺寸是直接原
// 样返回(见 decode.h "已经不小于原图时直接返回原图的拷贝")，所以这张
// "原图"就是 compute_dhash 实际拿去算哈希的那张，不会被重采样改变。
DecodedImage make_dhash_source(ImageHash target_hash) {
  DecodedImage img;
  img.width = 9;
  img.height = 8;
  img.rgba.resize(9 * 8 * 4, 255);
  int bit = 0;
  for (int y = 0; y < 8; ++y) {
    int value = 128;
    set_luminance(img, 0, y, value);
    for (int x = 0; x < 8; ++x) {
      bool one = (target_hash >> bit) & 1;
      value = one ? value - 5 : value + 5;
      set_luminance(img, x + 1, y, value);
      ++bit;
    }
  }
  return img;
}

// 给 detail::find_duplicates_impl 用的假 decode_fn：path -> hash 直接查
// 表返回一张 make_dhash_source(hash) 构造出的图片；查不到的 path 返回
// DecodeFailed，模拟"这张图解码失败"。
detail::PreviewDecodeFn hash_map_decoder(std::unordered_map<std::string, ImageHash> by_path) {
  return [by_path = std::move(by_path)](const std::string& path) {
    auto it = by_path.find(path);
    if (it == by_path.end()) {
      return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);
    }
    return Result<DecodedImage, DecodeError>::Ok(make_dhash_source(it->second));
  };
}

std::string path_for(const Fixture& fx, char name) {
  return fx.root_path + "/" + std::string(1, name) + ".jpg";
}

}  // namespace

TEST_CASE("compute_dhash reproduces the exact bit pattern used to construct the source image") {
  CHECK(compute_dhash(make_dhash_source(0)) == 0);
  CHECK(compute_dhash(make_dhash_source(~ImageHash{0})) == ~ImageHash{0});
  ImageHash mixed = 0x123456789ABCDEF0ULL;
  CHECK(compute_dhash(make_dhash_source(mixed)) == mixed);
}

TEST_CASE("hamming_distance counts differing bits") {
  ImageHash h = 0x0F0F0F0F0F0F0F0FULL;
  CHECK(hamming_distance(h, h) == 0);
  CHECK(hamming_distance(h, ~h) == 64);
  CHECK(hamming_distance(ImageHash{0}, ImageHash{0b111}) == 3);
}

TEST_CASE("images with identical hashes inside the time window are grouped; keep_id falls back to "
          "captured_at when unevaluated") {
  auto fx = make_fixture("same_hash_grouped", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);
  set_captured_at(fx.db, fx.images[2], 1008);  // 最新拍的，全组都没评估时应该被选中保留

  ImageHash h = 0xAAAAAAAAAAAAAAAAULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}, {path_for(fx, 'c'), h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, /*time_window_seconds=*/10,
                                              /*hash_threshold=*/5, nullptr, decoder);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].image_ids.size() == 3);
  CHECK(groups[0].keep_id == fx.images[2]);
}

TEST_CASE("images outside the time window are never compared, even with identical hashes") {
  auto fx = make_fixture("time_window", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1011);  // 差 11 秒，超过默认 10 秒窗口

  ImageHash h = 0x5555555555555555ULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  CHECK(groups.empty());
}

TEST_CASE("visually different images inside the time window are not grouped") {
  auto fx = make_fixture("different_hash", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);

  ImageHash h = 0;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), ~h}});  // 距离 64

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  CHECK(groups.empty());
}

TEST_CASE("union-find merges transitively: A-B and B-C close, A-C over threshold, still one group") {
  auto fx = make_fixture("transitive", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);
  set_captured_at(fx.db, fx.images[2], 1004);

  ImageHash hash_a = 0;
  ImageHash hash_b = 0b111;          // 跟 a 距离 3(<=5)
  ImageHash hash_c = 0b111111;       // 跟 b 距离 3(<=5)，跟 a 距离 6(>5)
  auto decoder = hash_map_decoder(
      {{path_for(fx, 'a'), hash_a}, {path_for(fx, 'b'), hash_b}, {path_for(fx, 'c'), hash_c}});

  REQUIRE(hamming_distance(hash_a, hash_c) > 5);

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].image_ids.size() == 3);
}

TEST_CASE("keep_id picks the highest overall_score when every group member is evaluated") {
  auto fx = make_fixture("keep_by_score", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);
  set_captured_at(fx.db, fx.images[2], 1004);  // 时间上最新，但评分不是最高，不应该被选中

  insert_evaluation(fx.db, fx.images[0], 9, 9, 9);  // overall 9,最高
  insert_evaluation(fx.db, fx.images[1], 3, 3, 3);
  insert_evaluation(fx.db, fx.images[2], 6, 6, 6);

  ImageHash h = 0x0F0F0F0F0F0F0F0FULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}, {path_for(fx, 'c'), h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].keep_id == fx.images[0]);
}

TEST_CASE("keep_id falls back to captured_at when even one group member is unevaluated") {
  auto fx = make_fixture("keep_by_time_fallback", 3);
  set_captured_at(fx.db, fx.images[0], 1000);  // 评分最高，但时间最老
  set_captured_at(fx.db, fx.images[1], 1002);  // 完全没评估过
  set_captured_at(fx.db, fx.images[2], 1004);  // 评分不是最高，但时间最新——组内有未评估成员时应该选它

  insert_evaluation(fx.db, fx.images[0], 9, 9, 9);
  // fx.images[1] 不插入评估记录
  insert_evaluation(fx.db, fx.images[2], 3, 3, 3);

  ImageHash h = 0xCCCCCCCCCCCCCCCCULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}, {path_for(fx, 'c'), h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].keep_id == fx.images[2]);
}

TEST_CASE("a decode failure excludes that image from grouping without affecting the rest") {
  auto fx = make_fixture("decode_failure", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);
  set_captured_at(fx.db, fx.images[2], 1004);

  ImageHash h = 0x3333333333333333ULL;
  // 'b' 故意不注册进 decoder,模拟解码失败
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'c'), h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  REQUIRE(groups.size() == 1);
  const auto& members = groups[0].image_ids;
  CHECK(members.size() == 2);
  // 顺序不保证跟 fx.images 下标一致(images 表的 id 分配顺序不保证是文件
  // 名字典序)，只检查成员集合本身——'b' 解码失败，不该出现在组里。
  CHECK(std::find(members.begin(), members.end(), fx.images[0]) != members.end());
  CHECK(std::find(members.begin(), members.end(), fx.images[2]) != members.end());
  CHECK(std::find(members.begin(), members.end(), fx.images[1]) == members.end());
}

TEST_CASE("images with no captured_at never participate in clustering") {
  auto fx = make_fixture("no_captured_at", 2);
  // 都不设置 captured_at,保持 NULL

  ImageHash h = 0x1111111111111111ULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  CHECK(groups.empty());
}

TEST_CASE("raw images with a preview cache resolve to the cache path, not root_path + file_path") {
  auto fx = make_fixture("raw_cache_path", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);

  std::string cache_path = fx.root_path + "/cache/b_preview.jpg";
  mark_raw_with_cache(fx.db, fx.images[1], cache_path);

  ImageHash h = 0x2222222222222222ULL;
  // 'b' 只在 cache_path 下注册,如果 resolve_path 算错(仍然拼 root+file_path)
  // 就会解码失败,分不到组里。
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {cache_path, h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].image_ids.size() == 2);
}

TEST_CASE("on_progress reports one callback per candidate cluster, including singletons") {
  auto fx = make_fixture("progress", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);   // 跟 a 同一簇
  set_captured_at(fx.db, fx.images[2], 5000);   // 单独一簇

  ImageHash h = 0x4444444444444444ULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}, {path_for(fx, 'c'), h}});

  std::vector<std::pair<int, int>> calls;
  auto on_progress = [&](int done, int total) { calls.push_back({done, total}); };

  detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, on_progress, decoder);
  REQUIRE(calls.size() == 2);  // 两个候选簇:{a,b} 和 {c}
  CHECK(calls[0] == std::pair<int, int>{1, 2});
  CHECK(calls[1] == std::pair<int, int>{2, 2});
}

TEST_CASE("find_duplicates (public entry point) wires the real decode path without crashing on an "
          "empty scope") {
  auto fx = make_fixture("public_entry_empty", 1);
  auto groups = find_duplicates(fx.db, fx.root_path, {});
  CHECK(groups.empty());
}
