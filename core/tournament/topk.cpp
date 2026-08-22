#include "core/tournament/topk.h"

#include <algorithm>
#include <utility>

namespace pzt::core::tournament {

namespace {

// 需要多少轮才能把 n 个人淘汰到只剩 1 个：每轮 ceil(n/2)，也就是
// ceil(log2 n)。同时就是树的深度，因而是「多取一名」的路径长度上界。
int ceil_log2(int n) {
  int depth = 0;
  while ((1 << depth) < n) ++depth;
  return depth;
}

}  // namespace

int max_comparisons_for_top_k(int member_count, int k) {
  if (member_count <= 1) return 0;
  int wanted = std::clamp(k, 0, member_count);
  if (wanted <= 0) return 0;
  return (member_count - 1) + (wanted - 1) * ceil_log2(member_count);
}

TopKSelection select_top_k(db::Database&, const std::string&, const std::vector<project::ImageId>&, int,
                           const dedup::detail::PreviewDecodeFn&, const detail::CompareFn&) {
  return TopKSelection{};
}

namespace detail {

LoserTree::LoserTree(int member_count) {
  if (member_count <= 0) return;

  nodes_.resize(static_cast<std::size_t>(member_count));  // 叶子：下标即成员下标
  winner_.resize(static_cast<std::size_t>(member_count));
  for (int i = 0; i < member_count; ++i) winner_[static_cast<std::size_t>(i)] = i;

  // 逐轮建内部节点，配对规则与 tournament.cpp 的 bracket 推进一致：相邻
  // 两个位置相比，奇数个时最后一个轮空(不建节点，原样进下一轮)。内部节
  // 点总是在它的两个孩子之后被 push_back，所以 nodes_ 的下标顺序天然就
  // 是一个可以正序扫描的拓扑序。
  std::vector<int> round(static_cast<std::size_t>(member_count));
  for (int i = 0; i < member_count; ++i) round[static_cast<std::size_t>(i)] = i;

  while (round.size() > 1) {
    std::vector<int> next_round;
    next_round.reserve((round.size() + 1) / 2);
    for (std::size_t i = 0; i < round.size(); i += 2) {
      if (i + 1 < round.size()) {
        int node = static_cast<int>(nodes_.size());
        nodes_.push_back(Node{round[i], round[i + 1], -1});
        winner_.push_back(-1);
        nodes_[static_cast<std::size_t>(round[i])].parent = node;
        nodes_[static_cast<std::size_t>(round[i + 1])].parent = node;
        next_round.push_back(node);
      } else {
        next_round.push_back(round[i]);  // 轮空，直接进下一轮
      }
    }
    round = std::move(next_round);
  }
  root_ = round.front();
}

Extracted LoserTree::extract_next(const IndexCompareFn& compare) {
  if (root_ < 0) return Extracted{ExtractStatus::Exhausted};

  if (!seeded_) {
    // 第一次取名次时才把整棵树比出来(m-1 场)。建树本身不发起比较，所以
    // m == 1 时这个循环一次都不进，零比较直接出结果。
    for (std::size_t node = 0; node < nodes_.size(); ++node) {
      if (nodes_[node].left < 0) continue;  // 叶子
      int left = winner_[static_cast<std::size_t>(nodes_[node].left)];
      int right = winner_[static_cast<std::size_t>(nodes_[node].right)];
      auto result = compare(left, right);
      if (!result) return Extracted{ExtractStatus::Aborted};
      winner_[node] = *result == ComparisonWinner::Left ? left : right;
    }
    seeded_ = true;
  } else {
    // 上一名的摘除推迟到这里做，不是在上次返回之前做完：一名取到手就顺
    // 手补位的话，第 1 名要花 (m-1) + 一条路径，k 名合计
    // (m-1) + k*ceil(log2 m)，比 max_comparisons_for_top_k 报给用户的上
    // 界正好多出一条路径。推迟之后第 1 名恰好 m-1 场、之后每名至多一条
    // 路径，合计才落在 (m-1) + (k-1)*ceil(log2 m) 上。
    winner_[static_cast<std::size_t>(last_taken_)] = -1;
    for (int node = last_taken_; nodes_[static_cast<std::size_t>(node)].parent >= 0;) {
      int parent = nodes_[static_cast<std::size_t>(node)].parent;
      int left = winner_[static_cast<std::size_t>(nodes_[static_cast<std::size_t>(parent)].left)];
      int right = winner_[static_cast<std::size_t>(nodes_[static_cast<std::size_t>(parent)].right)];

      if (left < 0 || right < 0) {
        // 有一边已经空了就不用比：活着的那个直接上，两边都空则这棵子树
        // 也空。这正是"实际次数会比上界少"的来源。
        winner_[static_cast<std::size_t>(parent)] = left < 0 ? right : left;
      } else {
        auto result = compare(left, right);
        if (!result) return Extracted{ExtractStatus::Aborted};
        winner_[static_cast<std::size_t>(parent)] = *result == ComparisonWinner::Left ? left : right;
      }
      node = parent;
    }
  }

  int taken = winner_[static_cast<std::size_t>(root_)];
  if (taken < 0) return Extracted{ExtractStatus::Exhausted};
  last_taken_ = taken;
  return Extracted{ExtractStatus::Ok, taken};
}

}  // namespace detail

}  // namespace pzt::core::tournament
