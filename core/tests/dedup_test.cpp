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
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::Result;
using pzt::core::db::Database;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::DecodeError;
using pzt::core::decode::encode_jpeg_file;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using pzt::core::project::ProjectNotFoundError;
using namespace pzt::core::dedup;
using namespace pzt::core::tagging;

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

TEST_CASE("keep_id breaks a score tie by keeping the most recently captured image") {
  auto fx = make_fixture("keep_by_score_tie", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1004);  // 分数并列最高的两张里,时间更新的这张应该被选中
  set_captured_at(fx.db, fx.images[2], 1002);

  insert_evaluation(fx.db, fx.images[0], 7, 7, 7);
  insert_evaluation(fx.db, fx.images[1], 7, 7, 7);  // 跟 a 同分,但更新,应该保留这张
  insert_evaluation(fx.db, fx.images[2], 3, 3, 3);

  ImageHash h = 0x5A5A5A5A5A5A5A5AULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}, {path_for(fx, 'c'), h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].keep_id == fx.images[1]);
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

// ---------------------------------------------------------------------
// find_and_tag_duplicates：编排层，走真实的默认 decode_fn(不注入假的)，
// 所以这几个用例需要真的 JPEG 字节——make_fixture 的 touch() 只建空文
// 件，这里用 encode_jpeg_file 覆盖成一张真正能解码的纯色图。两张字节完
// 全相同的 JPEG 解码结果必然逐像素相同，dHash 距离必然是 0，不需要精确
// 控制压缩细节就能稳定制造"这是一组重复"的场景——跟
// core/tests/decode_test.cpp 的思路一致，只是换成 encode_jpeg_file 而不
// 是手写 CGImageDestination。
bool write_solid_jpeg(const fs::path& path, int width, int height, unsigned char gray) {
  DecodedImage img;
  img.width = width;
  img.height = height;
  img.rgba.assign(static_cast<std::size_t>(width) * height * 4, gray);
  return encode_jpeg_file(img, path.string()).ok();
}

bool has_duplicate_tag(Database& db, ImageId id, TagId duplicate_tag_id) {
  for (const auto& t : tags_for_image(db, id)) {
    if (t.id == duplicate_tag_id) return true;
  }
  return false;
}

TEST_CASE("find_and_tag_duplicates tags non-keep members and reports a correct summary") {
  auto fx = make_fixture("facade_basic", 2);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));  // 字节相同,必然判为重复
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);  // 更新,全组都没评估时应该被保留

  auto result = find_and_tag_duplicates(fx.db, fx.project_id, fx.images);
  REQUIRE(result.ok());
  CHECK(result.value().group_count == 1);
  CHECK(result.value().tagged_count == 1);
  CHECK(result.value().unevaluated_image_count == 2);

  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[1], duplicate_tag_id));  // 保留的那张不该被打标签
  CHECK(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));
}

TEST_CASE("find_and_tag_duplicates only clears old marks inside the requested scope") {
  auto fx = make_fixture("facade_scope", 4);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 200));
  REQUIRE(write_solid_jpeg(dir / "d.jpg", 16, 16, 200));
  set_captured_at(fx.db, fx.images[0], 1000);  // a,b:一簇
  set_captured_at(fx.db, fx.images[1], 1002);
  set_captured_at(fx.db, fx.images[2], 5000);  // c,d:另一簇,时间上跟 a,b 离得远
  set_captured_at(fx.db, fx.images[3], 5002);

  // 模拟"上一次某个更大范围/不同范围的运行"给 c 留下的标记——这次的 scope
  // 不包含 c,d,不该动它。
  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[2], duplicate_tag_id).ok());

  std::vector<ImageId> scope = {fx.images[0], fx.images[1]};  // 只有 a,b
  auto result = find_and_tag_duplicates(fx.db, fx.project_id, scope);
  REQUIRE(result.ok());
  CHECK(result.value().group_count == 1);      // 只看到 a,b 这一组
  CHECK(result.value().tagged_count == 1);
  CHECK(result.value().unevaluated_image_count == 2);  // 只统计 scope 内的

  CHECK(has_duplicate_tag(fx.db, fx.images[2], duplicate_tag_id));  // c 在 scope 外,标记原样保留
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[3], duplicate_tag_id));  // d 从没被碰过
}

TEST_CASE("find_and_tag_duplicates clears stale marks before re-tagging on re-run") {
  auto fx = make_fixture("facade_rerun", 2);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);  // b 更新,第一次跑应该保留 b

  auto first = find_and_tag_duplicates(fx.db, fx.project_id, fx.images);
  REQUIRE(first.ok());
  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  CHECK(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[1], duplicate_tag_id));

  // 反转拍摄时间,这次 a 应该变成被保留的那张——差值仍然在默认 10 秒时间
  // 窗口内,不能像 1000 vs 2000 那样把两张图直接拆进两个不同候选簇。
  set_captured_at(fx.db, fx.images[0], 1005);
  set_captured_at(fx.db, fx.images[1], 1000);

  auto second = find_and_tag_duplicates(fx.db, fx.project_id, fx.images);
  REQUIRE(second.ok());
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));  // 旧标记被清掉了
  CHECK(has_duplicate_tag(fx.db, fx.images[1], duplicate_tag_id));        // 新一轮的非保留项
}

TEST_CASE("find_and_tag_duplicates returns ProjectNotFoundError for an unknown project") {
  auto fx = make_fixture("facade_missing_project", 1);
  auto result = find_and_tag_duplicates(fx.db, fx.project_id + 999999, fx.images);
  REQUIRE_FALSE(result.ok());
  CHECK(result.error() == ProjectNotFoundError::NotFound);
}
