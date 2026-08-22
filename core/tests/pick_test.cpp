#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/db/database.h"
#include "core/pick/pick.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
namespace dedup = pzt::core::dedup;
using namespace pzt::core::pick;
using pzt::core::Result;
using pzt::core::db::Database;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::DecodeError;
using pzt::core::dedup::ImageHash;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using pzt::core::tagging::add_tag;
using pzt::core::tagging::ensure_duplicate_tag;
using pzt::core::tagging::ensure_reject_tag;
using pzt::core::tagging::images_with_tag;

namespace {

// 这几个 helper 跟 tournament_test.cpp / dedup_test.cpp 的同名 helper 是
// 同一个模式：各测试文件独立一份，不专门开共用头文件收这几行（见
// tournament_test.cpp 上的说明）。
std::string fresh_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / ("pick_" + tag + ".db")).string();
  fs::remove(path);
  fs::remove(path + "-wal");
  fs::remove(path + "-shm");
  return path;
}

fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / ("pick_" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void set_luminance(DecodedImage& img, int x, int y, int value) {
  std::size_t idx =
      (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)) * 4;
  img.rgba[idx + 0] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 1] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 2] = static_cast<std::uint8_t>(value);
  img.rgba[idx + 3] = 255;
}

// 反向构造一张 9x8 合成图片使 compute_dhash(...) 精确等于 target_hash。
// 分簇看的是 dHash，比较那一头也靠它反查"这张是谁"，所以一张图的哈希在
// 这个测试文件里同时是它的身份。
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
    std::ofstream f(photos / (name + ".jpg"), std::ios::binary);
    f << "x";
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

std::string path_for(const Fixture& fx, char name) {
  return fx.root_path + "/" + std::string(1, name) + ".jpg";
}

dedup::detail::PreviewDecodeFn hash_map_decoder(std::unordered_map<std::string, ImageHash> by_path) {
  return [by_path = std::move(by_path)](const std::string& path) {
    auto it = by_path.find(path);
    if (it == by_path.end()) {
      return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);
    }
    return Result<DecodedImage, DecodeError>::Ok(make_dhash_source(it->second));
  };
}

// 假的"人"：按 rank_by_hash 里的偏好选，同时记下每一场比的是哪两张。
// 哈希在这个文件里就是图片的身份，所以断言"某张一次都没上过场"只需要看
// 这份记录。
struct FakeHuman {
  explicit FakeHuman(std::unordered_map<ImageHash, int> ranks) : rank_by_hash(std::move(ranks)) {}

  std::unordered_map<ImageHash, int> rank_by_hash;
  int calls = 0;
  std::set<ImageHash> seen;
  int abort_after = -1;  // >= 0 时第 abort_after+1 场放弃(返回 nullopt)

  std::optional<ComparisonWinner> operator()(const DecodedImage& a, const DecodedImage& b) {
    auto ha = dedup::compute_dhash(a);
    auto hb = dedup::compute_dhash(b);
    REQUIRE(ha.has_value());
    REQUIRE(hb.has_value());
    if (abort_after >= 0 && calls >= abort_after) return std::nullopt;
    ++calls;
    seen.insert(*ha);
    seen.insert(*hb);
    return rank_by_hash.at(*ha) > rank_by_hash.at(*hb) ? ComparisonWinner::Left
                                                        : ComparisonWinner::Right;
  }
};

// 六张图，两个多图簇 + 一个单例：
//   a,b,c 同一场景(1000..1002)，d,e 同一场景(2000..2001)，f 自己一张
//   (3000)。偏好 d(50) > f(45) > e(40) > b(30) > c(20) > a(10)。
// 于是 C=6、m=3(两个簇冠军 b/d 加单例 f)、第一级恰好 3 场。
struct SixImageScene {
  Fixture fx;
  std::unordered_map<char, ImageHash> hash_by_name;
  dedup::detail::PreviewDecodeFn decoder;
  FakeHuman human;
};

SixImageScene make_six_image_scene(const std::string& tag) {
  auto fx = make_fixture(tag, 6);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1001);
  set_captured_at(fx.db, fx.images[2], 1002);
  set_captured_at(fx.db, fx.images[3], 2000);
  set_captured_at(fx.db, fx.images[4], 2001);
  set_captured_at(fx.db, fx.images[5], 3000);

  // 簇内的哈希两两只差一两位(阈值内)，跨簇差半个字长(阈值外)。
  ImageHash base_one = 0x0F0F0F0F0F0F0F0FULL;
  ImageHash base_two = 0xF0F0F0F0F0F0F0F0ULL;
  std::unordered_map<char, ImageHash> hash_by_name{
      {'a', base_one},         {'b', base_one ^ 0x1ULL},  {'c', base_one ^ 0x3ULL},
      {'d', base_two},         {'e', base_two ^ 0x10ULL}, {'f', 0x00000000FFFFFFFFULL},
  };

  std::unordered_map<std::string, ImageHash> by_path;
  for (auto [name, hash] : hash_by_name) by_path[path_for(fx, name)] = hash;

  std::unordered_map<ImageHash, int> ranks{
      {hash_by_name['a'], 10}, {hash_by_name['b'], 30}, {hash_by_name['c'], 20},
      {hash_by_name['d'], 50}, {hash_by_name['e'], 40}, {hash_by_name['f'], 45},
  };

  auto decoder = hash_map_decoder(by_path);
  return SixImageScene{std::move(fx), hash_by_name, std::move(decoder), FakeHuman{std::move(ranks)}};
}

CompareFn as_compare_fn(FakeHuman& human) {
  return [&human](const DecodedImage& a, const DecodedImage& b) { return human(a, b); };
}

std::set<ImageId> rejected_ids(Fixture& fx) {
  auto tag = pzt::core::tagging::find_tag_by_name(fx.db, fx.project_id, pzt::core::tagging::kRejectTagName);
  if (!tag) return {};
  auto tagged = images_with_tag(fx.db, fx.images, *tag);
  return std::set<ImageId>(tagged.begin(), tagged.end());
}

constexpr int kTimeWindow = 20;
constexpr int kHashThreshold = 10;

}  // namespace

TEST_CASE("pick 留下 min(N,m) 张，其余全部打上废片标签") {
  auto scene = make_six_image_scene("full_run");
  auto& fx = scene.fx;

  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/2, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(scene.human));
  REQUIRE(result.ok());
  const auto& value = result.value();

  CHECK_FALSE(value.insufficient_candidates);
  CHECK_FALSE(value.declined);
  CHECK_FALSE(value.cancelled);
  CHECK(value.cost.candidate_count == 6);
  CHECK(value.cost.champion_count == 3);
  CHECK(value.cost.first_stage_comparisons == 3);  // C - m
  CHECK(value.cost.reject_count == 4);             // C - min(N, m)

  // 簇冠军是 b(30>20>10) 与 d(50>40)，加上单例 f；决赛偏好 d > f > b。
  REQUIRE(value.selected.size() == 2);
  CHECK(value.selected[0] == fx.images[3]);  // d
  CHECK(value.selected[1] == fx.images[5]);  // f

  CHECK(value.rejected_count == 4);
  std::set<ImageId> expected_rejected{fx.images[0], fx.images[1], fx.images[2], fx.images[4]};
  CHECK(rejected_ids(fx) == expected_rejected);

  CHECK(value.comparisons_done == scene.human.calls);
  CHECK(value.comparisons_done <= value.cost.max_comparisons);
}

TEST_CASE("第一级比较次数恰好 C - m，两级进度报的是同一批数") {
  auto scene = make_six_image_scene("progress");
  auto& fx = scene.fx;

  std::vector<PickProgress> progress;
  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/2, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(scene.human), nullptr,
                                   [&progress](const PickProgress& p) { progress.push_back(p); });
  REQUIRE(result.ok());

  int cluster_stage = 0;
  int final_stage = 0;
  for (const auto& p : progress) {
    (p.stage == PickStage::Cluster ? cluster_stage : final_stage)++;
    CHECK(p.max_comparisons == result.value().cost.max_comparisons);
  }
  CHECK(cluster_stage == result.value().cost.first_stage_comparisons);
  CHECK(cluster_stage == 3);
  CHECK(final_stage > 0);
  CHECK(static_cast<int>(progress.size()) == result.value().comparisons_done);

  // 计数是 1-based 且连续，分母是各自那一级的总数。
  for (std::size_t i = 0; i < progress.size(); ++i) {
    CHECK(progress[i].comparisons_done == static_cast<int>(i) + 1);
  }
  CHECK(progress.front().stage == PickStage::Cluster);
  CHECK(progress.front().group_index == 1);
  CHECK(progress.front().group_total == 2);  // 只数要跑比较的组，单例不占一格
  CHECK(progress.front().match_index == 1);
  CHECK(progress.front().match_total == 2);  // 三个成员的簇比两场
  CHECK(progress.back().stage == PickStage::Final);
  CHECK(progress.back().rank_total == 2);  // min(N, m)

  // 一级的字段不许漏到另一级：渲染那一层按 stage 取字段，残值会直接画在
  // 屏幕上。
  for (const auto& p : progress) {
    if (p.stage == PickStage::Cluster) {
      CHECK(p.rank_index == 0);
      CHECK(p.rank_total == 0);
    } else {
      CHECK(p.group_index == 0);
      CHECK(p.group_total == 0);
      CHECK(p.match_index == 0);
      CHECK(p.match_total == 0);
    }
  }
}

TEST_CASE("带废片或重复标签的图片一次都不进入比较") {
  auto scene = make_six_image_scene("excluded");
  auto& fx = scene.fx;
  REQUIRE(add_tag(fx.db, fx.images[2], ensure_reject_tag(fx.db, fx.project_id)).ok());     // c
  REQUIRE(add_tag(fx.db, fx.images[4], ensure_duplicate_tag(fx.db, fx.project_id)).ok());  // e

  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/2, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(scene.human));
  REQUIRE(result.ok());

  CHECK(result.value().cost.candidate_count == 4);  // a,b,d,f
  CHECK(scene.human.seen.count(scene.hash_by_name['c']) == 0);
  CHECK(scene.human.seen.count(scene.hash_by_name['e']) == 0);
  // 已经带着重复标签的 e 不会因为落选而多打一次废片：它压根不是候选。
  CHECK(rejected_ids(fx).count(fx.images[4]) == 0);
}

TEST_CASE("一个成员全是废片的连拍不产生任何比较") {
  auto scene = make_six_image_scene("burst_all_rejected");
  auto& fx = scene.fx;
  auto reject = ensure_reject_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[1], reject).ok());  // b
  REQUIRE(add_tag(fx.db, fx.images[2], reject).ok());  // c

  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/2, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(scene.human));
  REQUIRE(result.ok());

  // 剩下的 a 变成单例，那一簇根本不成形：第一级只剩 d/e 的一场。
  CHECK(result.value().cost.candidate_count == 4);
  CHECK(result.value().cost.champion_count == 3);
  CHECK(result.value().cost.first_stage_comparisons == 1);
  CHECK(scene.human.seen.count(scene.hash_by_name['b']) == 0);
  CHECK(scene.human.seen.count(scene.hash_by_name['c']) == 0);
}

TEST_CASE("N >= m 时跳过第二级，不从各簇亚军补位") {
  auto scene = make_six_image_scene("skip_final");
  auto& fx = scene.fx;

  std::vector<PickProgress> progress;
  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/5, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(scene.human), nullptr,
                                   [&progress](const PickProgress& p) { progress.push_back(p); });
  REQUIRE(result.ok());
  const auto& value = result.value();

  // 要 5 张只拿到 3 张，另外 3 张判废 - 不是把各簇的第二名挖出来凑数。
  CHECK(value.selected.size() == 3);
  CHECK(value.cost.reject_count == 3);
  CHECK(value.rejected_count == 3);
  CHECK(value.comparisons_done == 3);  // 只有第一级
  CHECK(value.cost.max_comparisons == 3);
  for (const auto& p : progress) CHECK(p.stage == PickStage::Cluster);
  std::set<ImageId> selected(value.selected.begin(), value.selected.end());
  CHECK(selected == std::set<ImageId>{fx.images[1], fx.images[3], fx.images[5]});  // b, d, f
}

TEST_CASE("单例直接进第二级，不产生任何簇内比较") {
  auto fx = make_fixture("all_singletons", 3);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);
  set_captured_at(fx.db, fx.images[2], 9000);
  std::unordered_map<char, ImageHash> hash_by_name{{'a', 0x1ULL}, {'b', 0x2ULL}, {'c', 0x4ULL}};
  std::unordered_map<std::string, ImageHash> by_path;
  for (auto [name, hash] : hash_by_name) by_path[path_for(fx, name)] = hash;
  FakeHuman human{{{hash_by_name['a'], 10}, {hash_by_name['b'], 30}, {hash_by_name['c'], 20}}};

  std::vector<PickProgress> progress;
  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/1, kTimeWindow,
                                   kHashThreshold, hash_map_decoder(by_path), as_compare_fn(human),
                                   nullptr, [&progress](const PickProgress& p) { progress.push_back(p); });
  REQUIRE(result.ok());

  CHECK(result.value().cost.champion_count == 3);
  CHECK(result.value().cost.first_stage_comparisons == 0);
  REQUIRE(result.value().selected.size() == 1);
  CHECK(result.value().selected[0] == fx.images[1]);  // b
  for (const auto& p : progress) CHECK(p.stage == PickStage::Final);
}

TEST_CASE("C <= N 时零比较零写入，报候选不足") {
  auto scene = make_six_image_scene("insufficient");
  auto& fx = scene.fx;

  bool gate_called = false;
  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/6, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(scene.human),
                                   [&gate_called](const PickCost&) {
                                     gate_called = true;
                                     return true;
                                   });
  REQUIRE(result.ok());

  CHECK(result.value().insufficient_candidates);
  CHECK(result.value().selected.empty());
  CHECK(result.value().cost.candidate_count == 6);
  CHECK(result.value().rejected_count == 0);
  CHECK(scene.human.calls == 0);
  CHECK_FALSE(gate_called);  // 没有要确认的开销就不问
  CHECK(rejected_ids(fx).empty());
}

TEST_CASE("C == 0 与 count 非正同样落在候选不足那条短路上") {
  auto scene = make_six_image_scene("zero_candidates");
  auto& fx = scene.fx;
  auto reject = ensure_reject_tag(fx.db, fx.project_id);
  for (auto id : fx.images) REQUIRE(add_tag(fx.db, id, reject).ok());

  auto empty_pool = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/2, kTimeWindow,
                                       kHashThreshold, scene.decoder, as_compare_fn(scene.human));
  REQUIRE(empty_pool.ok());
  CHECK(empty_pool.value().insufficient_candidates);
  CHECK(empty_pool.value().cost.candidate_count == 0);
  CHECK(scene.human.calls == 0);

  auto other = make_six_image_scene("non_positive_count");
  auto zero = detail::pick_impl(other.fx.db, other.fx.project_id, other.fx.images, /*count=*/0,
                                 kTimeWindow, kHashThreshold, other.decoder, as_compare_fn(other.human));
  REQUIRE(zero.ok());
  CHECK(zero.value().insufficient_candidates);
  CHECK(other.human.calls == 0);
  CHECK(rejected_ids(other.fx).empty());
}

TEST_CASE("闸门拿到精确开销，返回 false 时零比较零写入") {
  auto scene = make_six_image_scene("gate_declined");
  auto& fx = scene.fx;

  PickCost seen{};
  int gate_calls = 0;
  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/2, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(scene.human),
                                   [&](const PickCost& cost) {
                                     ++gate_calls;
                                     seen = cost;
                                     return false;
                                   });
  REQUIRE(result.ok());

  CHECK(gate_calls == 1);  // 问一次，问在任何一次比较之前
  CHECK(seen.candidate_count == 6);
  CHECK(seen.champion_count == 3);
  CHECK(seen.first_stage_comparisons == 3);
  CHECK(seen.reject_count == 4);
  CHECK(seen.max_comparisons == 3 + 2 + 1 * 2);  // (C-m) + (m-1) + (N-1)*ceil(log2 m)

  CHECK(result.value().declined);
  CHECK_FALSE(result.value().cancelled);
  CHECK(result.value().selected.empty());
  CHECK(scene.human.calls == 0);
  CHECK(rejected_ids(fx).empty());
}

TEST_CASE("中途取消：零写入，且与闸门拒绝是两个可区分的结果") {
  auto scene = make_six_image_scene("cancelled");
  auto& fx = scene.fx;

  bool cancel_requested = false;
  auto result = detail::pick_impl(
      fx.db, fx.project_id, fx.images, /*count=*/2, kTimeWindow, kHashThreshold, scene.decoder,
      as_compare_fn(scene.human), nullptr,
      [&cancel_requested](const PickProgress& p) {
        if (p.comparisons_done == 2) cancel_requested = true;  // 第二场之后喊停
      },
      [&cancel_requested] { return cancel_requested; });  // 粘性
  REQUIRE(result.ok());

  CHECK(result.value().cancelled);
  CHECK_FALSE(result.value().declined);
  CHECK(result.value().selected.empty());
  CHECK(result.value().rejected_count == 0);
  CHECK(rejected_ids(fx).empty());
  CHECK(scene.human.calls < 3);  // 喊停之后不再比
  // 取消之后这批数仍然要拿得到："已经比过的 K 次将全部作废"这句话要用它。
  CHECK(result.value().cost.candidate_count == 6);
  CHECK(result.value().cost.champion_count == 3);
  CHECK(result.value().comparisons_done == scene.human.calls);
}

TEST_CASE("比较原语中途放弃等同于取消：零写入") {
  auto scene = make_six_image_scene("compare_gives_up");
  auto& fx = scene.fx;
  scene.human.abort_after = 2;

  auto result = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/2, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(scene.human));
  REQUIRE(result.ok());

  CHECK(result.value().cancelled);
  CHECK(result.value().selected.empty());
  CHECK(rejected_ids(fx).empty());
}

TEST_CASE("重跑是收敛的：第二次的候选恰好是第一次留下的那批") {
  auto scene = make_six_image_scene("rerun");
  auto& fx = scene.fx;

  auto first = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/3, kTimeWindow,
                                  kHashThreshold, scene.decoder, as_compare_fn(scene.human));
  REQUIRE(first.ok());
  REQUIRE(first.value().selected.size() == 3);
  std::set<ImageId> kept(first.value().selected.begin(), first.value().selected.end());

  FakeHuman second_human{scene.human.rank_by_hash};
  auto second = detail::pick_impl(fx.db, fx.project_id, fx.images, /*count=*/1, kTimeWindow,
                                   kHashThreshold, scene.decoder, as_compare_fn(second_human));
  REQUIRE(second.ok());

  // 第二次是在第一次留下的 3 张里选，不是重新洗一遍全库。
  CHECK(second.value().cost.candidate_count == 3);
  CHECK(second.value().selected.size() == 1);
  CHECK(kept.count(second.value().selected[0]) == 1);
  auto still_kept = kept;
  for (auto id : rejected_ids(fx)) still_kept.erase(id);
  CHECK(still_kept.size() == 1);
  CHECK(*still_kept.begin() == second.value().selected[0]);
}

TEST_CASE("批量落库整批失败时报错，不谎称判废了 0 张") {
  auto scene = make_six_image_scene("write_failed");
  auto& fx = scene.fx;

  // 混进一张别的项目的图片：它进得了候选(排除规则只看标签)，也能作为单例
  // 当上簇冠军，但落库时批量接口会认出它不属于废片标签所在的项目、整批拒
  // 绝。这是"选完了却一张都没记下来"最容易够到的一条真实路径。
  auto other_photos = fresh_photo_dir("write_failed_other");
  std::ofstream(other_photos / "z.jpg", std::ios::binary) << "x";
  auto other = create_project(fx.db, "other", other_photos.string());
  REQUIRE(other.ok());
  auto foreign = find_image_by_path(fx.db, other.value(), "z.jpg");
  REQUIRE(foreign.has_value());

  auto ids = fx.images;
  ids.push_back(*foreign);
  auto result = detail::pick_impl(fx.db, fx.project_id, ids, /*count=*/2, kTimeWindow, kHashThreshold,
                                  scene.decoder, as_compare_fn(scene.human));

  REQUIRE_FALSE(result.ok());
  CHECK(result.error() == PickError::RejectTagWriteFailed);
  CHECK(rejected_ids(fx).empty());  // 全有全无：一张都没打上
}
