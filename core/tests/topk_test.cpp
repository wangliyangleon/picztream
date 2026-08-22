#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/tournament/topk.h"

namespace fs = std::filesystem;
namespace dedup = pzt::core::dedup;
using namespace pzt::core::tournament;
using pzt::core::Result;
using pzt::core::db::Database;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::DecodeError;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using pzt::core::tournament::detail::ComparisonWinner;
using pzt::core::tournament::detail::ExtractStatus;
using pzt::core::tournament::detail::IndexCompareFn;
using pzt::core::tournament::detail::LoserTree;

namespace {

int ceil_log2(int n) {
  int d = 0;
  while ((1 << d) < n) ++d;
  return d;
}

// 全序的假比较函数：values[i] 大的赢。同时记比较次数，用来断言上界。
// 全序保证了"取出的前 k 名"有一个唯一正确答案可以对照 - 败者树的严格
// 正确性本来就只在全序下成立(见 topk.h 的正确性前提)。
struct TotalOrderCompare {
  std::vector<int> values;
  int calls = 0;

  std::optional<ComparisonWinner> operator()(int left, int right) {
    ++calls;
    return values[left] > values[right] ? ComparisonWinner::Left : ComparisonWinner::Right;
  }
};

// 按 values 降序排出来的成员下标，就是 top-k 的正确答案。
std::vector<int> expected_ranking(const std::vector<int>& values) {
  std::vector<int> idx(values.size());
  std::iota(idx.begin(), idx.end(), 0);
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return values[a] > values[b]; });
  return idx;
}

IndexCompareFn wrap(TotalOrderCompare& cmp) {
  return [&cmp](int l, int r) { return cmp(l, r); };
}

}  // namespace

TEST_CASE("max_comparisons_for_top_k 给出 (m-1)+(k-1)*ceil(log2 m)") {
  CHECK(max_comparisons_for_top_k(1, 1) == 0);
  CHECK(max_comparisons_for_top_k(2, 1) == 1);
  CHECK(max_comparisons_for_top_k(2, 2) == 2);
  CHECK(max_comparisons_for_top_k(8, 1) == 7);
  CHECK(max_comparisons_for_top_k(8, 8) == 7 + 7 * 3);
  // PRD 里那个 200 张分 40 组取 20 张的算例：第二级 39 + 19*6。
  CHECK(max_comparisons_for_top_k(40, 20) == 39 + 19 * 6);
}

TEST_CASE("max_comparisons_for_top_k 的边界：k 夹到 [0,m]，空池为 0") {
  CHECK(max_comparisons_for_top_k(0, 5) == 0);
  CHECK(max_comparisons_for_top_k(1, 5) == 0);
  CHECK(max_comparisons_for_top_k(8, 0) == 0);
  CHECK(max_comparisons_for_top_k(8, -3) == 0);
  // k > m 按 k == m 算，不会因为多要而报出更大的上界。
  CHECK(max_comparisons_for_top_k(8, 99) == max_comparisons_for_top_k(8, 8));
}

TEST_CASE("全序下取出的前 k 名与真实排序逐个相等") {
  // 轮空分布不同的几个规模都过一遍：2 的幂、奇数、非 2 的幂。
  for (int m : {1, 2, 3, 5, 8, 13, 16, 40}) {
    std::vector<int> values(m);
    // 一个乱序但确定的排列，避免"恰好按下标顺序"掩盖配对错误。
    for (int i = 0; i < m; ++i) values[i] = (i * 37 + 11) % 101;

    TotalOrderCompare cmp{values};
    LoserTree tree(m);

    std::vector<int> got;
    for (int i = 0; i < m; ++i) {
      auto e = tree.extract_next(wrap(cmp));
      REQUIRE(e.status == ExtractStatus::Ok);
      got.push_back(e.member);
    }
    CHECK(got == expected_ranking(values));

    // 取完之后再取一次是 Exhausted，不是崩溃也不是重复给最后一名。
    CHECK(tree.extract_next(wrap(cmp)).status == ExtractStatus::Exhausted);
  }
}

TEST_CASE("建树一次 m-1 场，之后每取一名至多 ceil(log2 m) 场") {
  for (int m : {2, 3, 5, 8, 13, 16, 40}) {
    std::vector<int> values(m);
    for (int i = 0; i < m; ++i) values[i] = (i * 37 + 11) % 101;

    TotalOrderCompare cmp{values};
    LoserTree tree(m);

    auto first = tree.extract_next(wrap(cmp));
    REQUIRE(first.status == ExtractStatus::Ok);
    CHECK(cmp.calls == m - 1);  // 单淘汰 m 个成员恰好 m-1 场

    for (int k = 2; k <= m; ++k) {
      int before = cmp.calls;
      auto e = tree.extract_next(wrap(cmp));
      REQUIRE(e.status == ExtractStatus::Ok);
      CHECK(cmp.calls - before <= ceil_log2(m));
    }

    CHECK(cmp.calls <= max_comparisons_for_top_k(m, m));
  }
}

TEST_CASE("m == 1 时零比较直接出结果") {
  TotalOrderCompare cmp{{42}};
  LoserTree tree(1);

  auto e = tree.extract_next(wrap(cmp));
  REQUIRE(e.status == ExtractStatus::Ok);
  CHECK(e.member == 0);
  CHECK(cmp.calls == 0);
  CHECK(tree.extract_next(wrap(cmp)).status == ExtractStatus::Exhausted);
}

TEST_CASE("空池直接 Exhausted") {
  TotalOrderCompare cmp{{}};
  LoserTree tree(0);
  CHECK(tree.extract_next(wrap(cmp)).status == ExtractStatus::Exhausted);
}

TEST_CASE("比较函数返回 nullopt 时整场作废") {
  LoserTree tree(8);
  IndexCompareFn give_up = [](int, int) { return std::optional<ComparisonWinner>{}; };
  CHECK(tree.extract_next(give_up).status == ExtractStatus::Aborted);
}

TEST_CASE("取前 k 名时总场次不超过 max_comparisons_for_top_k 报出的上界") {
  const int m = 40;
  const int k = 20;
  std::vector<int> values(m);
  for (int i = 0; i < m; ++i) values[i] = (i * 37 + 11) % 101;

  TotalOrderCompare cmp{values};
  LoserTree tree(m);

  std::vector<int> got;
  for (int i = 0; i < k; ++i) {
    auto e = tree.extract_next(wrap(cmp));
    REQUIRE(e.status == ExtractStatus::Ok);
    got.push_back(e.member);
  }

  auto expected = expected_ranking(values);
  expected.resize(k);
  CHECK(got == expected);
  CHECK(cmp.calls <= max_comparisons_for_top_k(m, k));
}

// ---------------------------------------------------------------------------
// select_top_k：把上面那棵树接上真实的 ImageId、解码与比较原语。这一段
// 测的是解码失败的三条语义，树本身的性质已经在上面测过了。
// ---------------------------------------------------------------------------

namespace {

// 跟 tournament_test.cpp / dedup_test.cpp 的同名 helper 同一个模式：各测
// 试文件独立一份，不专门开共用头文件收这几行(见 tournament_test.cpp 上
// 的说明)。
std::string fresh_topk_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / ("topk_" + tag + ".db")).string();
  fs::remove(path);
  fs::remove(path + "-wal");
  fs::remove(path + "-shm");
  return path;
}

fs::path fresh_topk_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / ("topk_" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

struct TopKFixture {
  Database db;
  ProjectId project_id;
  std::vector<ImageId> images;  // images[0]="a.jpg", images[1]="b.jpg", ...
  std::string root_path;
};

TopKFixture make_topk_fixture(const std::string& tag, int image_count) {
  auto db = Database::open_at(fresh_topk_db_path(tag));
  auto photos = fresh_topk_photo_dir(tag);
  for (int i = 0; i < image_count; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    auto p = photos / (name + ".jpg");
    std::ofstream f(p, std::ios::binary);
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
  return TopKFixture{std::move(db), created.value(), std::move(images), photos.string()};
}

void set_topk_captured_at(Database& db, ImageId id, std::optional<std::int64_t> value) {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "UPDATE images SET captured_at = ? WHERE id = ?;", -1, &stmt, nullptr);
  if (value) {
    sqlite3_bind_int64(stmt, 1, *value);
  } else {
    sqlite3_bind_null(stmt, 1);
  }
  sqlite3_bind_int64(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

std::string topk_path_for(const TopKFixture& fx, char name) {
  return fx.root_path + "/" + std::string(1, name) + ".jpg";
}

// 假 decode_fn：一张 1x1 图片，红通道存这张图的"好看程度"，配合下面的
// 假 compare_fn 就能让胜负完全可预测。fail_paths 里的路径返回解码失败。
// 不碰真实 JPEG，也不需要 dHash - 这一层根本不分簇。
dedup::detail::PreviewDecodeFn rank_decoder(std::unordered_map<std::string, int> rank_by_path,
                                             std::set<std::string> fail_paths) {
  return [rank_by_path = std::move(rank_by_path),
          fail_paths = std::move(fail_paths)](const std::string& path) {
    if (fail_paths.count(path) > 0) {
      return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);
    }
    auto it = rank_by_path.find(path);
    if (it == rank_by_path.end()) {
      return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);
    }
    DecodedImage img;
    img.width = 1;
    img.height = 1;
    img.rgba = {static_cast<std::uint8_t>(it->second), 0, 0, 255};
    return Result<DecodedImage, DecodeError>::Ok(std::move(img));
  };
}

// 红通道大的赢，并记调用次数：解码失败的那几对根本不该走到这里。
struct RankCompare {
  int calls = 0;
  std::optional<ComparisonWinner> operator()(const DecodedImage& a, const DecodedImage& b) {
    ++calls;
    return a.rgba[0] > b.rgba[0] ? ComparisonWinner::Left : ComparisonWinner::Right;
  }
};

}  // namespace

TEST_CASE("select_top_k 按比较结果取出前 k 名") {
  auto fx = make_topk_fixture("plain", 5);
  // a..e 的"好看程度"：c > a > e > b > d
  std::unordered_map<std::string, int> ranks{{topk_path_for(fx, 'a'), 40}, {topk_path_for(fx, 'b'), 20},
                                             {topk_path_for(fx, 'c'), 50}, {topk_path_for(fx, 'd'), 10},
                                             {topk_path_for(fx, 'e'), 30}};
  RankCompare cmp;
  auto result = select_top_k(fx.db, fx.root_path, fx.images, 3, rank_decoder(ranks, {}),
                             [&cmp](const DecodedImage& a, const DecodedImage& b) { return cmp(a, b); });

  CHECK_FALSE(result.aborted);
  REQUIRE(result.ranked.size() == 3);
  CHECK(result.ranked[0] == fx.images[2]);  // c
  CHECK(result.ranked[1] == fx.images[0]);  // a
  CHECK(result.ranked[2] == fx.images[4]);  // e
  CHECK(cmp.calls <= max_comparisons_for_top_k(5, 3));
}

TEST_CASE("k 大于成员数时取完全部，k 为 0 或成员为空时零比较") {
  auto fx = make_topk_fixture("clamp", 3);
  std::unordered_map<std::string, int> ranks{
      {topk_path_for(fx, 'a'), 10}, {topk_path_for(fx, 'b'), 30}, {topk_path_for(fx, 'c'), 20}};

  RankCompare cmp;
  auto all = select_top_k(fx.db, fx.root_path, fx.images, 99, rank_decoder(ranks, {}),
                          [&cmp](const DecodedImage& a, const DecodedImage& b) { return cmp(a, b); });
  REQUIRE(all.ranked.size() == 3);
  CHECK(all.ranked[0] == fx.images[1]);

  RankCompare none;
  auto zero = select_top_k(fx.db, fx.root_path, fx.images, 0, rank_decoder(ranks, {}),
                           [&none](const DecodedImage& a, const DecodedImage& b) { return none(a, b); });
  CHECK(zero.ranked.empty());
  CHECK(none.calls == 0);

  RankCompare empty_cmp;
  auto empty = select_top_k(fx.db, fx.root_path, {}, 3, rank_decoder(ranks, {}),
                            [&empty_cmp](const DecodedImage& a, const DecodedImage& b) { return empty_cmp(a, b); });
  CHECK(empty.ranked.empty());
  CHECK(empty_cmp.calls == 0);
}

TEST_CASE("一对里一张解码失败：对手晋级，比较继续，不整簇退化") {
  auto fx = make_topk_fixture("one_fail", 4);
  // b 解码失败。其余三张 c > a > d。
  std::unordered_map<std::string, int> ranks{{topk_path_for(fx, 'a'), 40},
                                             {topk_path_for(fx, 'c'), 50},
                                             {topk_path_for(fx, 'd'), 10}};
  RankCompare cmp;
  auto result = select_top_k(fx.db, fx.root_path, fx.images, 3, rank_decoder(ranks, {topk_path_for(fx, 'b')}),
                             [&cmp](const DecodedImage& a, const DecodedImage& b) { return cmp(a, b); });

  CHECK_FALSE(result.aborted);
  // 整簇退化的话这里会是空的/或者按 keep_id 排 - 它没有退化，正常出了
  // 三名，且 b 一场没赢过所以排在能解码的三张之后。
  REQUIRE(result.ranked.size() == 3);
  CHECK(result.ranked[0] == fx.images[2]);  // c
  CHECK(result.ranked[1] == fx.images[0]);  // a
  CHECK(result.ranked[2] == fx.images[3]);  // d
  // a vs b 那一对没有送进 compare_fn：只画得出一张的时候不需要问用户。
  CHECK(cmp.calls < max_comparisons_for_top_k(4, 3));
}

TEST_CASE("一对里两张都解码失败：captured_at 较新者晋级") {
  auto fx = make_topk_fixture("both_fail_time", 2);
  set_topk_captured_at(fx.db, fx.images[0], 1000);
  set_topk_captured_at(fx.db, fx.images[1], 2000);  // b 更新

  RankCompare cmp;
  auto result = select_top_k(fx.db, fx.root_path, fx.images, 2, rank_decoder({}, {}),
                             [&cmp](const DecodedImage& a, const DecodedImage& b) { return cmp(a, b); });

  CHECK_FALSE(result.aborted);
  REQUIRE(result.ranked.size() == 2);
  CHECK(result.ranked[0] == fx.images[1]);  // b，captured_at 更新
  CHECK(cmp.calls == 0);                    // 一张都画不出来，不问用户
}

TEST_CASE("两张都解码失败且 captured_at 相同：id 较小者晋级") {
  auto fx = make_topk_fixture("both_fail_id", 2);
  set_topk_captured_at(fx.db, fx.images[0], 1500);
  set_topk_captured_at(fx.db, fx.images[1], 1500);

  RankCompare cmp;
  auto result = select_top_k(fx.db, fx.root_path, fx.images, 2, rank_decoder({}, {}),
                             [&cmp](const DecodedImage& a, const DecodedImage& b) { return cmp(a, b); });

  REQUIRE(result.ranked.size() == 2);
  CHECK(result.ranked[0] == std::min(fx.images[0], fx.images[1]));
  CHECK(cmp.calls == 0);
}

TEST_CASE("两张都解码失败且 captured_at 缺失：有时间的一方晋级，都没有则按 id") {
  auto fx = make_topk_fixture("both_fail_null", 2);
  set_topk_captured_at(fx.db, fx.images[0], std::nullopt);
  set_topk_captured_at(fx.db, fx.images[1], 900);

  RankCompare cmp;
  auto with_time = select_top_k(fx.db, fx.root_path, fx.images, 1, rank_decoder({}, {}),
                                [&cmp](const DecodedImage& a, const DecodedImage& b) { return cmp(a, b); });
  REQUIRE(with_time.ranked.size() == 1);
  CHECK(with_time.ranked[0] == fx.images[1]);

  set_topk_captured_at(fx.db, fx.images[1], std::nullopt);
  auto both_null = select_top_k(fx.db, fx.root_path, fx.images, 1, rank_decoder({}, {}),
                                [&cmp](const DecodedImage& a, const DecodedImage& b) { return cmp(a, b); });
  REQUIRE(both_null.ranked.size() == 1);
  CHECK(both_null.ranked[0] == std::min(fx.images[0], fx.images[1]));
  CHECK(cmp.calls == 0);
}

TEST_CASE("compare_fn 中途放弃时整场作废、名次为空") {
  auto fx = make_topk_fixture("abort", 4);
  std::unordered_map<std::string, int> ranks{{topk_path_for(fx, 'a'), 40}, {topk_path_for(fx, 'b'), 20},
                                             {topk_path_for(fx, 'c'), 50}, {topk_path_for(fx, 'd'), 10}};

  auto result = select_top_k(fx.db, fx.root_path, fx.images, 2, rank_decoder(ranks, {}),
                             [](const DecodedImage&, const DecodedImage&) {
                               return std::optional<ComparisonWinner>{};
                             });

  CHECK(result.aborted);
  CHECK(result.ranked.empty());
}
