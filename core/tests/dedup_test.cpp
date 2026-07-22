#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

#include "core/dedup/dedup.h"
#include "core/db/database.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::Result;
using pzt::core::ai::Provider;
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

// 跟 compare_test.cpp/evaluation_test.cpp 的 EnvVarGuard 是同一个写法——
// 各自文件独立一份，见那边的说明。
struct EnvVarGuard {
  std::string name;
  std::optional<std::string> previous;

  EnvVarGuard(std::string n, const char* value) : name(std::move(n)) {
    const char* existing = std::getenv(name.c_str());
    if (existing) previous = existing;
    if (value) {
      setenv(name.c_str(), value, 1);
    } else {
      unsetenv(name.c_str());
    }
  }

  ~EnvVarGuard() {
    if (previous) {
      setenv(name.c_str(), previous->c_str(), 1);
    } else {
      unsetenv(name.c_str());
    }
  }
};

}  // namespace

TEST_CASE("compute_dhash reproduces the exact bit pattern used to construct the source image") {
  // F-36：compute_dhash 现返回 optional(resize 失败时 nullopt);9x8 合成图必
  // 然成功,解包断言精确 bit pattern。
  REQUIRE(compute_dhash(make_dhash_source(0)).value() == 0);
  REQUIRE(compute_dhash(make_dhash_source(~ImageHash{0})).value() == ~ImageHash{0});
  ImageHash mixed = 0x123456789ABCDEF0ULL;
  REQUIRE(compute_dhash(make_dhash_source(mixed)).value() == mixed);
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

// F-39：同一候选簇里分出多个重复组时，输出顺序以前跟着 unordered_map 的
// 遍历序走、跨进程运行不稳定，违反 Dedup PRD 的确定性 NFR 字面。现在每簇
// 的组按组内最小 id 升序输出——这里在一个时间簇内造两组({a,b} 同哈希、
// {c,d} 另一个哈希、两组间距离超阈值不合并)，直接验证返回的组按 front(即
// 组内最小 id)升序，不依赖 images 表实际的 id 分配顺序。
TEST_CASE("groups from one cluster come out ordered by their smallest image id (deterministic)") {
  auto fx = make_fixture("group_order_deterministic", 4);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);
  set_captured_at(fx.db, fx.images[2], 1004);
  set_captured_at(fx.db, fx.images[3], 1006);  // 四张都在默认 10 秒窗口内，同一候选簇

  ImageHash h1 = 0;
  ImageHash h2 = ~ImageHash{0};  // 跟 h1 距离 64，两组之间绝不合并
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h1},
                                    {path_for(fx, 'b'), h1},
                                    {path_for(fx, 'c'), h2},
                                    {path_for(fx, 'd'), h2}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  REQUIRE(groups.size() == 2);
  // 每组内部升序(已有保证)，且组间按组内最小 id 升序(F-39)。
  CHECK(std::is_sorted(groups[0].image_ids.begin(), groups[0].image_ids.end()));
  CHECK(std::is_sorted(groups[1].image_ids.begin(), groups[1].image_ids.end()));
  CHECK(groups[0].image_ids.front() < groups[1].image_ids.front());
}

// W2026-07-21：keep 统一"留最新"，不再依赖评估分数——这里只验证纯时间
// 规则(评估记录已从 dedup 剥离)。全组都没评估，时间最新的 images[2] 被
// 选中保留；分数场景整块删除。captured_at 打平兜底 id 最小的极端分支由
// 上面的 grouping fallback 用例覆盖。
TEST_CASE("keep_id keeps the most recently captured image regardless of evaluation") {
  auto fx = make_fixture("keep_by_time", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1004);  // 时间最新，应被保留
  set_captured_at(fx.db, fx.images[2], 1002);

  ImageHash h = 0x5A5A5A5A5A5A5A5AULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}, {path_for(fx, 'c'), h}});

  auto groups = detail::find_duplicates_impl(fx.db, fx.root_path, fx.images, 10, 5, nullptr, decoder);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].keep_id == fx.images[1]);
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

  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[1], duplicate_tag_id));  // 保留的那张不该被打标签
  CHECK(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));
}

// W2026-07-21：聚类前排除废片。keep 改成"留最新"后，一张废片若 captured_at
// 最新会成为 keep 把它的好邻居打成重复。a(好图) 和 b(同字节近邻,更新) 本会
// 成一组，但 b 被打了废片标签——b 应被排除在聚类之外，a 落单不成组、不被
// 打成重复。
TEST_CASE("find_and_tag_duplicates excludes reject-tagged images from clustering") {
  auto fx = make_fixture("facade_exclude_reject", 2);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));  // 字节相同,本会跟 a 成一组
  set_captured_at(fx.db, fx.images[0], 1000);  // a: 好图
  set_captured_at(fx.db, fx.images[1], 1002);  // b: 更新,但被标废片

  auto reject_tag_id = ensure_reject_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[1], reject_tag_id).ok());

  auto result = find_and_tag_duplicates(fx.db, fx.project_id, fx.images);
  REQUIRE(result.ok());
  CHECK(result.value().group_count == 0);  // b 被排除,a 落单,不成组

  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));  // 好图 a 不被打成重复
}

// F-18：以前不检查 add_tag 的返回值，tagged_count 无条件自增。如果用户
// 在这个功能之前就手动建过一个带 cap 的同名"重复"标签(ensure_duplicate_
// tag 会直接复用它，不区分是不是系统创建的，见该函数的说明)，超出 cap
// 的图实际打不上标签，汇总却照样报"打了 N 张"——这里用 cap=0 制造一个
// 必然失败的场景，验证 tagged_count 现在如实反映真正打上的数量。
TEST_CASE("find_and_tag_duplicates only counts images that actually got tagged, not cap conflicts") {
  auto fx = make_fixture("facade_cap_conflict", 2);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));  // 字节相同,必然判为重复
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);

  REQUIRE(create_tag(fx.db, fx.project_id, kDuplicateTagName, /*cap=*/0, /*is_ordered=*/false,
                      /*is_system=*/true)
              .ok());

  auto result = find_and_tag_duplicates(fx.db, fx.project_id, fx.images);
  REQUIRE(result.ok());
  CHECK(result.value().group_count == 1);
  CHECK(result.value().tagged_count == 0);  // add_tag 因 cap=0 必然失败，不该照样报 1

  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));  // 确实没打上
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

// F-08：范围内 captured_at 为 NULL 的图片(微信图/截图/编辑过的导出件常
// 见)完全不参与任何比较,以前被静默忽略,现在要如实报数量。
TEST_CASE("find_and_tag_duplicates reports skipped_no_capture_time for images with no captured_at") {
  auto fx = make_fixture("facade_skipped", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);
  // fx.images[2] 故意不设置 captured_at,保持 NULL。

  auto result = find_and_tag_duplicates(fx.db, fx.project_id, fx.images);
  REQUIRE(result.ok());
  CHECK(result.value().skipped_no_capture_time == 1);
}

TEST_CASE("find_and_tag_duplicates skipped_no_capture_time is 0 when every image has a capture time") {
  auto fx = make_fixture("facade_skipped_zero", 2);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);

  auto result = find_and_tag_duplicates(fx.db, fx.project_id, fx.images);
  REQUIRE(result.ok());
  CHECK(result.value().skipped_no_capture_time == 0);
}

// F-08：time_window_seconds/hash_threshold 以前在 find_and_tag_duplicates
// 里写死 10/5,这次改成显式参数——用时间窗口的边界验证参数真的从门面
// 一路传到了算法层,不是摆设。两张字节相同的图(hamming 距离必为 0,
// hash_threshold 不是这里的变量)拍摄时间差 20 秒:默认 10 秒时间窗把
// 它们拆进两个候选簇,谁都不跟谁比较,不成组;显式传 30 秒时间窗则落进
// 同一簇,成组。
TEST_CASE("find_and_tag_duplicates honors an explicit time_window_seconds instead of the old hardcoded 10") {
  auto fx = make_fixture("facade_time_window_param", 2);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1020);  // 差 20 秒

  auto default_window = find_and_tag_duplicates(fx.db, fx.project_id, fx.images);
  REQUIRE(default_window.ok());
  CHECK(default_window.value().group_count == 0);  // 默认 10 秒,拆进两个候选簇

  auto wider_window =
      find_and_tag_duplicates(fx.db, fx.project_id, fx.images, /*time_window_seconds=*/30);
  REQUIRE(wider_window.ok());
  CHECK(wider_window.value().group_count == 1);  // 显式传 30 秒,落进同一候选簇
}

TEST_CASE("find_and_tag_duplicates returns ProjectNotFoundError for an unknown project") {
  auto fx = make_fixture("facade_missing_project", 1);
  auto result = find_and_tag_duplicates(fx.db, fx.project_id + 999999, fx.images);
  REQUIRE_FALSE(result.ok());
  CHECK(result.error() == ProjectNotFoundError::NotFound);
}

// W2026-07-21 目标二：ai_enabled=true 时走真实的 tournament::
// cluster_and_choose(不是注入假 compare_fn)。用 evaluation_test.cpp/
// compare_test.cpp 同一个技巧——Provider::Claude 在 ANTHROPIC_API_KEY 没
// 设时，request_comparison 在真正发起网络请求之前就确定性地返回
// MissingApiKey，不连真网络。这条用例验证的是"ai_enabled 真的传到底
// 了"这条真实链路：每个簇因为 AI 失败退化成跟 ai_enabled=false 完全一样
// 的 keep_id 选择，group_count/tagged_count 不变，只是多一个
// ai_fallback_count 反映退化了几组。
TEST_CASE("find_and_tag_duplicates with ai_enabled=true degrades to keep_id when the provider has no "
          "credentials (no real network call)") {
  EnvVarGuard key("ANTHROPIC_API_KEY", nullptr);
  auto fx = make_fixture("facade_ai_fallback", 2);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));  // 字节相同,必然判为重复
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);  // 更新,是 keep_id 规则下的答案

  auto result = find_and_tag_duplicates(fx.db, fx.project_id, fx.images, 10, 5, nullptr,
                                         /*ai_enabled=*/true, Provider::Claude);
  REQUIRE(result.ok());
  CHECK(result.value().group_count == 1);
  CHECK(result.value().tagged_count == 1);       // 跟 ai_enabled=false 时完全一致(退化成 keep_id)
  CHECK(result.value().ai_fallback_count == 1);  // 唯一那组因为没 key 而退化

  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[1], duplicate_tag_id));  // 保留最新的那张
  CHECK(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));
}
