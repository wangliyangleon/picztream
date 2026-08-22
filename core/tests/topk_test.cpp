#include <doctest.h>

#include <algorithm>
#include <numeric>
#include <optional>
#include <vector>

#include "core/tournament/topk.h"

using namespace pzt::core::tournament;
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
