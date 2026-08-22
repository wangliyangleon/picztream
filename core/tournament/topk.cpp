#include "core/tournament/topk.h"

#include <algorithm>
#include <utility>

#include "core/media/media.h"

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

namespace {

// 一个成员在整场里反复要用的那几样东西。像素不在里面 - 那是逐对惰性解
// 码的。
struct MemberMeta {
  project::ImageId id = 0;
  std::string preview_path;
  std::optional<std::int64_t> captured_at;
  // project::get_image 查得到这张图。查不到时当作"永远解码失败"，但 id
  // 仍然是调用方给的那个，所以下面的兜底规则照样能用。
  bool resolvable = false;
};

// 一对里两张都解码失败时谁晋级：captured_at 较新的赢，时间也相同则 id
// 较小的赢，与 core/dedup 挑 keep_id 的规则同一套。
//
// captured_at 缺失按"最旧"处理：有时间的一方赢，两边都没有时间就落到
// id 这条兜底上。dedup 那边挑 keep_id 时手上只有 captured_at 非 NULL
// 的图片，这里的候选池不保证这一点(没有拍摄时间的照片照样是候选)，所
// 以规则要多铺这一格才完整。
bool wins_without_pixels(const MemberMeta& left, const MemberMeta& right) {
  if (left.captured_at && right.captured_at) {
    if (*left.captured_at != *right.captured_at) return *left.captured_at > *right.captured_at;
    return left.id < right.id;
  }
  if (left.captured_at) return true;
  if (right.captured_at) return false;
  return left.id < right.id;
}

}  // namespace

TopKSelection select_top_k(db::Database& db, const std::string& root_path,
                           const std::vector<project::ImageId>& members, int k,
                           const dedup::detail::PreviewDecodeFn& decode_fn,
                           const detail::CompareFn& compare_fn) {
  TopKSelection selection;
  int member_count = static_cast<int>(members.size());
  int wanted = std::clamp(k, 0, member_count);
  if (wanted == 0) return selection;

  // 元数据一次查完：预览图路径与 captured_at 在整场里会被反复用到，每
  // 比一次再查一遍是白花的查询。像素仍然逐对惰性解码，跟
  // tournament.cpp 的 bracket 推进同一个模式 - 同一时刻最多两张解码图
  // 片存活，不随成员数变大。
  std::vector<MemberMeta> metas;
  metas.reserve(members.size());
  for (auto id : members) {
    MemberMeta meta;
    meta.id = id;
    if (auto info = project::get_image(db, id)) {
      meta.preview_path =
          media::resolve_preview_path(root_path, info->file_path, info->kind, info->preview_cache_path);
      meta.captured_at = info->captured_at;
      meta.resolvable = true;
    }
    metas.push_back(std::move(meta));
  }

  auto decode_at = [&](int index) -> std::optional<decode::DecodedImage> {
    const MemberMeta& meta = metas[static_cast<std::size_t>(index)];
    if (!meta.resolvable) return std::nullopt;
    auto decoded = decode_fn(meta.preview_path);
    if (!decoded.ok()) return std::nullopt;
    return decoded.value();
  };

  detail::IndexCompareFn compare = [&](int left, int right) -> std::optional<detail::ComparisonWinner> {
    auto left_image = decode_at(left);
    auto right_image = decode_at(right);
    if (left_image && right_image) return compare_fn(*left_image, *right_image);
    // 解码失败的一方判负。屏幕上只画得出一张，那张晋级是用户看到的画面
    // 唯一能支持的结论；问都不用问，所以这两支不进 compare_fn。
    if (left_image) return detail::ComparisonWinner::Left;
    if (right_image) return detail::ComparisonWinner::Right;
    return wins_without_pixels(metas[static_cast<std::size_t>(left)], metas[static_cast<std::size_t>(right)])
               ? detail::ComparisonWinner::Left
               : detail::ComparisonWinner::Right;
  };

  detail::LoserTree tree(member_count);
  selection.ranked.reserve(static_cast<std::size_t>(wanted));
  for (int taken = 0; taken < wanted; ++taken) {
    auto extracted = tree.extract_next(compare);
    if (extracted.status == detail::ExtractStatus::Aborted) {
      // 一场没比完的锦标赛没有名次可言：已经取到的几名一并丢掉，调用方
      // 按"这次什么都没发生"处理。
      selection.ranked.clear();
      selection.aborted = true;
      return selection;
    }
    if (extracted.status == detail::ExtractStatus::Exhausted) break;
    selection.ranked.push_back(members[static_cast<std::size_t>(extracted.member)]);
  }
  return selection;
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
