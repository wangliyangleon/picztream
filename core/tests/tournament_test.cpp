#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "core/tournament/tournament.h"
#include "core/db/database.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
namespace dedup = pzt::core::dedup;
using pzt::core::Result;
using pzt::core::ai::ComparisonResult;
using pzt::core::ai::CompareError;
using pzt::core::ai::LocalModelConfig;
using pzt::core::ai::Provider;
using pzt::core::db::Database;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::DecodeError;
using pzt::core::dedup::ImageHash;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::tagging;
using namespace pzt::core::tournament;

namespace {

std::string fresh_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / ("tournament_" + tag + ".db")).string();
  fs::remove(path);
  return path;
}

fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / ("tournament_" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << "x";
}

// 跟 dedup_test.cpp 的 make_fixture 同一个模式：建一个带 N 张图片(a.jpg,
// b.jpg, ...)的项目。
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
  sqlite3_prepare_v2(db.handle(), "UPDATE images SET captured_at = ? WHERE id = ?;", -1, &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, value);
  sqlite3_bind_int64(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void set_luminance(DecodedImage& img, int x, int y, int value) {
  std::size_t idx =
      (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)) * 4;
  img.rgba[idx + 0] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 1] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 2] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 3] = 255;
}

// 反向构造一张 9x8 合成图片使 compute_dhash(...) 精确等于 target_hash，跟
// dedup_test.cpp 的同名 helper 一致(dedup_test.cpp 是私有 helper，这里没
// 有共用头文件专门收这几行，各测试文件独立一份是这个代码库既有的惯例，
// 见 evaluation_test.cpp/style_test.cpp 的 EnvVarGuard 同样的说明)。
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

// 假 decode_fn：path -> hash 直接查表返回一张 make_dhash_source(hash) 构
// 造出的图片，供分簇(dHash 距离)和锦标赛(AI 比较拿到的"图片")两处共用同
// 一份注入——不需要真实 JPEG 文件。查不到的 path 返回 DecodeFailed。
dedup::detail::PreviewDecodeFn hash_map_decoder(std::unordered_map<std::string, ImageHash> by_path) {
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

bool has_duplicate_tag(Database& db, ImageId id, TagId duplicate_tag_id) {
  for (const auto& t : tags_for_image(db, id)) {
    if (t.id == duplicate_tag_id) return true;
  }
  return false;
}

// fake CompareFn：id 更大的那张赢(把 DecodedImage 里编码的哈希值当身份
// 用不方便，直接按传入顺序的调用计数从外层驱动更方便——这里改用一个更直
// 接的策略：谁的 dHash 更大谁赢，配合 make_dhash_source 精确可控)。同时
// 记录被调用的次数，用来断言"单例簇不发起任何比较"、"N 个成员恰好 N-1
// 次比较"。
struct FakeCompare {
  int calls = 0;

  Result<ComparisonResult, CompareError> operator()(const DecodedImage& a, const DecodedImage& b, Provider,
                                                      const LocalModelConfig&) {
    ++calls;
    // 复用 compute_dhash 反解出构造时的 target_hash——两张图都是
    // make_dhash_source 生成的，谁的哈希值更大谁赢，纯粹为了让胜负结果
    // 可预测、可断言。
    auto ha = dedup::compute_dhash(a);
    auto hb = dedup::compute_dhash(b);
    REQUIRE(ha.has_value());
    REQUIRE(hb.has_value());
    int winner = (*ha > *hb) ? 0 : 1;
    return Result<ComparisonResult, CompareError>::Ok(ComparisonResult{winner, "bigger hash wins"});
  }
};

// 每次比较都失败的 fake CompareFn，用来测 AI 失败退化。
Result<ComparisonResult, CompareError> always_fails(const DecodedImage&, const DecodedImage&, Provider,
                                                      const LocalModelConfig&) {
  return Result<ComparisonResult, CompareError>::Err(CompareError::NetworkError);
}

}  // namespace

TEST_CASE("cluster_and_choose with ai disabled: winner is the group's keep_id (captured_at newest)") {
  auto fx = make_fixture("ai_off_keep_newest", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);
  set_captured_at(fx.db, fx.images[2], 1008);  // 最新

  ImageHash h = 0xAAAAAAAAAAAAAAAAULL;
  auto decoder =
      hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}, {path_for(fx, 'c'), h}});
  FakeCompare fake_compare;

  auto result = detail::cluster_and_choose_impl(
      fx.db, fx.project_id, fx.images, /*time_window_seconds=*/10, /*hash_threshold=*/5, {},
      /*apply_dup_tag=*/false, /*ai_enabled=*/false, Provider::Local, LocalModelConfig{}, decoder,
      std::ref(fake_compare));
  REQUIRE(result.ok());
  REQUIRE(result.value().clusters.size() == 1);
  CHECK(result.value().clusters[0].members.size() == 3);
  CHECK(result.value().clusters[0].winner == fx.images[2]);
  CHECK(fake_compare.calls == 0);  // AI 关时完全不发起比较
  CHECK(result.value().ai_fallback_count == 0);
}

TEST_CASE("cluster_and_choose with ai enabled: bracket advances to a single winner, N-1 comparisons") {
  auto fx = make_fixture("ai_on_bracket", 5);
  for (int i = 0; i < 5; ++i) set_captured_at(fx.db, fx.images[i], 1000 + i);

  // 5 张哈希各不相同但两两距离都 <=5(阈值内)，全部聚成一组；哈希值刻意
  // 递增，配合 FakeCompare"哈希更大者赢"，可预测的 winner 是哈希最大的
  // 那张(images[4])。
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), 0},
                                    {path_for(fx, 'b'), 1},
                                    {path_for(fx, 'c'), 2},
                                    {path_for(fx, 'd'), 3},
                                    {path_for(fx, 'e'), 4}});
  FakeCompare fake_compare;

  auto result = detail::cluster_and_choose_impl(fx.db, fx.project_id, fx.images, 10, 5, {},
                                                 /*apply_dup_tag=*/false, /*ai_enabled=*/true, Provider::Local,
                                                 LocalModelConfig{}, decoder, std::ref(fake_compare));
  REQUIRE(result.ok());
  REQUIRE(result.value().clusters.size() == 1);
  CHECK(result.value().clusters[0].members.size() == 5);
  CHECK(result.value().clusters[0].winner == fx.images[4]);
  CHECK(fake_compare.calls == 4);  // 5 个成员，N-1 = 4 次比较
  CHECK(result.value().ai_fallback_count == 0);
}

TEST_CASE("cluster_and_choose: singleton candidates become their own trivial cluster, no AI call") {
  auto fx = make_fixture("singletons", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);  // 时间窗外，不会跟 a 聚在一起

  auto decoder = hash_map_decoder({{path_for(fx, 'a'), 0}, {path_for(fx, 'b'), 1}});
  FakeCompare fake_compare;

  auto result = detail::cluster_and_choose_impl(fx.db, fx.project_id, fx.images, 10, 5, {},
                                                 /*apply_dup_tag=*/false, /*ai_enabled=*/true, Provider::Local,
                                                 LocalModelConfig{}, decoder, std::ref(fake_compare));
  REQUIRE(result.ok());
  REQUIRE(result.value().clusters.size() == 2);
  for (const auto& c : result.value().clusters) {
    REQUIRE(c.members.size() == 1);
    CHECK(c.winner == c.members[0]);
  }
  CHECK(fake_compare.calls == 0);
}

TEST_CASE("cluster_and_choose: a failed comparison degrades just that cluster to keep_id") {
  auto fx = make_fixture("ai_fallback", 5);
  for (int i = 0; i < 5; ++i) set_captured_at(fx.db, fx.images[i], 1000 + i);
  // 前三张(a,b,c)彼此距离很近聚一组；后两张(d,e)彼此距离很近聚另一组，
  // 但两组之间(比如 a 和 d)距离远超阈值，不会被并查集传递合并成一组。
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), 0},
                                    {path_for(fx, 'b'), 1},
                                    {path_for(fx, 'c'), 3},
                                    {path_for(fx, 'd'), ~ImageHash{0}},
                                    {path_for(fx, 'e'), ~ImageHash{0} - 1}});

  auto result = detail::cluster_and_choose_impl(fx.db, fx.project_id, fx.images, 10, 5, {},
                                                 /*apply_dup_tag=*/false, /*ai_enabled=*/true, Provider::Local,
                                                 LocalModelConfig{}, decoder, always_fails);
  REQUIRE(result.ok());
  REQUIRE(result.value().clusters.size() == 2);
  // 两组都因为比较失败而退化，各自的 winner 应该等于"captured_at 最新"
  // (跟 AI 关时的规则一致)：a/b/c 组最新是 c，d/e 组最新是 e。
  for (const auto& c : result.value().clusters) {
    if (c.members.size() == 3) {
      CHECK(c.winner == fx.images[2]);  // c
    } else {
      REQUIRE(c.members.size() == 2);
      CHECK(c.winner == fx.images[4]);  // e
    }
  }
  CHECK(result.value().ai_fallback_count == 2);
}

TEST_CASE("cluster_and_choose: exclude_tag_names removes images tagged with any of the given names") {
  auto fx = make_fixture("exclude_tags", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1001);
  set_captured_at(fx.db, fx.images[2], 1002);

  auto reject_tag_id = ensure_reject_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[0], reject_tag_id).ok());
  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[1], duplicate_tag_id).ok());

  auto decoder = hash_map_decoder({{path_for(fx, 'a'), 0}, {path_for(fx, 'b'), 1}, {path_for(fx, 'c'), 2}});
  FakeCompare fake_compare;

  auto result = detail::cluster_and_choose_impl(
      fx.db, fx.project_id, fx.images, 10, 5, {kRejectTagName, kDuplicateTagName},
      /*apply_dup_tag=*/false, /*ai_enabled=*/false, Provider::Local, LocalModelConfig{}, decoder,
      std::ref(fake_compare));
  REQUIRE(result.ok());
  REQUIRE(result.value().clusters.size() == 1);  // 只剩 c
  CHECK(result.value().clusters[0].members == std::vector<ImageId>{fx.images[2]});
}

TEST_CASE("cluster_and_choose: apply_dup_tag=true tags non-winner members, re-run doesn't double-count") {
  auto fx = make_fixture("apply_dup_tag_true", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);
  set_captured_at(fx.db, fx.images[2], 1004);  // 最新，AI 关时是 winner

  ImageHash h = 0x5555555555555555ULL;
  auto decoder =
      hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}, {path_for(fx, 'c'), h}});
  FakeCompare fake_compare;

  auto run_once = [&] {
    return detail::cluster_and_choose_impl(fx.db, fx.project_id, fx.images, 10, 5, {},
                                            /*apply_dup_tag=*/true, /*ai_enabled=*/false, Provider::Local,
                                            LocalModelConfig{}, decoder, std::ref(fake_compare));
  };

  auto first = run_once();
  REQUIRE(first.ok());
  CHECK(first.value().tagged_count == 2);

  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  CHECK(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));
  CHECK(has_duplicate_tag(fx.db, fx.images[1], duplicate_tag_id));
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[2], duplicate_tag_id));  // winner 不被打标签

  auto second = run_once();
  REQUIRE(second.ok());
  CHECK(second.value().tagged_count == 2);  // 先清后打，不会累加成 4
}

TEST_CASE("cluster_and_choose: apply_dup_tag=false never tags anything even with a size>=2 cluster") {
  auto fx = make_fixture("apply_dup_tag_false", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);

  ImageHash h = 0x1111111111111111ULL;
  auto decoder = hash_map_decoder({{path_for(fx, 'a'), h}, {path_for(fx, 'b'), h}});
  FakeCompare fake_compare;

  auto result = detail::cluster_and_choose_impl(fx.db, fx.project_id, fx.images, 10, 5, {},
                                                 /*apply_dup_tag=*/false, /*ai_enabled=*/false, Provider::Local,
                                                 LocalModelConfig{}, decoder, std::ref(fake_compare));
  REQUIRE(result.ok());
  CHECK(result.value().tagged_count == 0);
  auto duplicate_tag_id = ensure_duplicate_tag(fx.db, fx.project_id);
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[0], duplicate_tag_id));
  CHECK_FALSE(has_duplicate_tag(fx.db, fx.images[1], duplicate_tag_id));
}

TEST_CASE("cluster_and_choose: images with no captured_at are counted as skipped but still appear as "
          "singletons") {
  auto fx = make_fixture("no_capture_time", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1002);
  // images[2] 保持 captured_at 为 NULL(不调用 set_captured_at)。

  auto decoder = hash_map_decoder({{path_for(fx, 'a'), 0}, {path_for(fx, 'b'), 1}});
  FakeCompare fake_compare;

  auto result = detail::cluster_and_choose_impl(fx.db, fx.project_id, fx.images, 10, 5, {},
                                                 /*apply_dup_tag=*/false, /*ai_enabled=*/false, Provider::Local,
                                                 LocalModelConfig{}, decoder, std::ref(fake_compare));
  REQUIRE(result.ok());
  CHECK(result.value().skipped_no_capture_time == 1);
  bool found_c_as_singleton = false;
  for (const auto& c : result.value().clusters) {
    if (c.members.size() == 1 && c.members[0] == fx.images[2]) found_c_as_singleton = true;
  }
  CHECK(found_c_as_singleton);
}

TEST_CASE("cluster_and_choose: unknown project_id returns ProjectNotFoundError without any query") {
  auto fx = make_fixture("unknown_project", 1);
  FakeCompare fake_compare;

  auto result = detail::cluster_and_choose_impl(fx.db, fx.project_id + 999999, fx.images, 10, 5, {},
                                                 /*apply_dup_tag=*/false, /*ai_enabled=*/false, Provider::Local,
                                                 LocalModelConfig{},
                                                 hash_map_decoder({}), std::ref(fake_compare));
  REQUIRE(!result.ok());
  CHECK(result.error() == pzt::core::project::ProjectNotFoundError::NotFound);
  CHECK(fake_compare.calls == 0);
}
