#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/dedup/dedup.h"
#include "core/decode/decode.h"
#include "core/project/project.h"
#include "core/tournament/tournament.h"

// 单淘汰只保证冠军正确：真正的亚军根本不在决赛里，它一定是输给过冠军的
// 那 ceil(log2 m) 个人之一。tournament.cpp 里的 bracket 推进每轮只把赢家
// 推进下一轮、败者信息当场丢弃，所以它只能出 top-1。
//
// 这一层把整棵 bracket 树留下来，并在其上提供「连续取第 1、2、…、k 名」
// 的能力：取下一名时只需重跑上一名走过的那条路，边际成本是
// ceil(log2 m) 次比较而不是 m 次。相对「摘掉冠军再重跑 k 遍单淘汰」
// (k*m 量级)，这是本层存在的全部理由。
namespace pzt::core::tournament {

// m 个成员取前 k 名所需比较次数的上界：(m-1) + (k-1)*ceil(log2 m)。
// 第一项是建树(单淘汰 m 个成员恰好 m-1 场)，第二项是之后每多取一名重跑
// 一条根到叶的路径。
//
// 这是**上界不是精确值**：轮空的那些位置不发起比较，实际次数会更少。要
// 的就是「开跑前算得出来」这个性质 - 调用方需要在一次比较都没发生之前
// 就告诉用户"最多需要 X 次"。
//
// k 会被夹到 [0, m]；m <= 1 或 k <= 0 时为 0(单个成员零比较直接出结果)。
int max_comparisons_for_top_k(int member_count, int k);

struct TopKSelection {
  // 第 1..k 名，按名次排列。成员不足时短于 k。
  std::vector<project::ImageId> ranked;
  // compare_fn 中途返回了 nullopt(调用方不比了)。为真时 ranked 为空 -
  // 一场没跑完的锦标赛没有名次可言，调用方应该按"这次什么都没发生"处理。
  bool aborted = false;
};

// 在 members 上跑一棵保留败者的 bracket 树，取出前 k 名。
//
// **正确性前提**：top-k 的严格正确性依赖比较关系是全序。人跨场景的判断
// 不满足这一条 - A 赢 B、B 赢 C、C 赢 A 会真实发生。这一层**接受它是一
// 个稳定的启发式排序**，不引入瑞士制那类抗矛盾的排名机制。产物是"哪 k
// 张留下"这个集合，不是一份要公示的名次表。
//
// 一次比较必须选出一个赢家：不做平局、不做双输。
//
// 解码失败的处理与 tournament.cpp 的 bracket 推进**有意不同**，那边任何
// 一张解码失败都让整簇退化成 keep_id：
//   - 一对里一张解码失败：**那一张判负**，对手直接晋级，比较继续。屏幕
//     上只画得出一张，那张晋级是用户看到的画面唯一能支持的结论；整簇退
//     化则意味着用户已经按过的比较全部白按，还会看到一个自己没选过的结
//     果进决赛。
//   - 一对里两张都解码失败：按 captured_at 较新的晋级，时间也相同则按
//     id 较小的，沿用 core/dedup 挑 keep_id 的同一套规则，不另立一套。
//     captured_at 缺失按"最旧"处理(有时间的一方胜出)，两边都缺失时同样
//     落到 id 较小的那条兜底上。
//
// decode_fn/compare_fn 都可注入，所以单测不需要真的解码 JPEG 或真的连
// 网络 - 跟 core/dedup 的 detail::find_duplicates_impl、core/ai/compare.h
// 的 detail::request_comparison_impl 是同一个模式。
//
// on_match_start：**每一场比较发起之前**(且在这一对的解码之前)调一次，
// 参数是"正在取第几名"与"取这一名过程中的第几场"，两个都是 1-based。
// 没有它的话调用方最细只能报到"取第几名"，而取第 1 名要跑完整棵树的
// m-1 场，那期间画面完全静止。放在解码之前调用，是为了让挂在这个钩子
// 里的取消检查能在解码这一对的开销发生之前生效。
//
// 返回 false = 别比了：与 compare_fn 返回 nullopt 走同一条出口
// (aborted=true、ranked 清空)。解码失败**不**经过这个钩子决出的胜负仍然
// 会先报一次 - 那一场真实发生过，只是没有人需要按键。
using MatchStartFn = std::function<bool(int rank_index, int match_index)>;

TopKSelection select_top_k(db::Database& db, const std::string& root_path,
                           const std::vector<project::ImageId>& members, int k,
                           const dedup::detail::PreviewDecodeFn& decode_fn,
                           const detail::CompareFn& compare_fn,
                           const MatchStartFn& on_match_start = nullptr);

namespace detail {

// 比较原语的纯下标版：只说"第 left 个和第 right 个谁赢"，不关心成员是
// 什么、这次比较由谁做出来。nullopt = 不比了，整场作废。
using IndexCompareFn = std::function<std::optional<ComparisonWinner>(int left, int right)>;

enum class ExtractStatus {
  Ok,         // member 是取出的这一名
  Exhausted,  // 树里已经没有成员了
  Aborted,    // IndexCompareFn 返回了 nullopt
};

struct Extracted {
  ExtractStatus status;
  int member = -1;  // status==Ok 时有效，是成员下标
};

// 保留败者的 bracket 树。名字里的"败者"指的是**败者留在树里**：内部节点
// 存的是自己这棵子树的赢家，输掉的人不出局、原地待命，成为下一名的来源。
// 这与经典 loser tree(内部节点存败者)存的东西不同，取名沿用这个项目里
// 「败者树」的既有叫法。
//
// 配对规则：同一轮里相邻两个位置相比，奇数个时最后一个轮空直接晋级。
// 这是这个项目里**唯一**一套 bracket 推进 - tournament.cpp 的 run_bracket
// 就是"建一棵树、取第 1 名"，不另外写一遍配对。这不是可以随手换的实现
// 细节：CompareFn 的契约里没有传递性，换一种配对方式可能真的换出一个不
// 同的赢家，两处各写一遍迟早会换出两个不同的答案。
//
// 建树本身不发起任何比较：第一次 extract_next 才把 m-1 场比完(内部节点
// 是按"子节点下标一定小于父节点"的顺序建的，所以一遍正序扫描就够，不需
// 要递归)。
//
// 之后每次 extract_next 先把**上一名**从叶子上摘掉、只重算它到根这一条
// 路上的节点(每层至多一场比较)，再读新的根。摘除推迟一次是为了对齐
// max_comparisons_for_top_k 报出的上界：取到第 1 名时只花了建树的 m-1
// 场，补位的那条路记在第 2 名头上，k 名合计才是
// (m-1) + (k-1)*ceil(log2 m)。
//
// **中途 Aborted 之后这棵树就废了**：那一场没比完，路径上的节点停在一个
// 算了一半的状态。之后再调 extract_next 只会继续拿到 Aborted，调用方该
// 把整个结果丢掉。
class LoserTree {
 public:
  explicit LoserTree(int member_count);

  Extracted extract_next(const IndexCompareFn& compare);

 private:
  struct Node {
    int left = -1;    // 内部节点必有两个孩子；叶子两个都是 -1
    int right = -1;
    int parent = -1;  // 根是 -1
  };

  // 节点与成员下标全程用 int，只在真的要索引 vector 时转一次。算法本身
  // 一行都不该被 static_cast 淹掉。
  Node& node_at(int index) { return nodes_[static_cast<std::size_t>(index)]; }
  const Node& node_at(int index) const { return nodes_[static_cast<std::size_t>(index)]; }
  int winner_at(int index) const { return winner_[static_cast<std::size_t>(index)]; }
  void set_winner(int index, int member) { winner_[static_cast<std::size_t>(index)] = member; }

  // 算出一个内部节点当前的赢家。返回 false = 比较函数放弃了。
  bool resolve(int node, const IndexCompareFn& compare);

  // 前 member_count 个是叶子，下标即成员下标；其后是内部节点。
  std::vector<Node> nodes_;
  // 每个节点当前的赢家(成员下标)，-1 = 这棵子树已经空了。
  std::vector<int> winner_;
  int root_ = -1;
  bool seeded_ = false;
  // 上一次取走的成员，它的摘除与补位推迟到下一次 extract_next 开头做。
  int last_taken_ = -1;
  // 放弃过一次就永远放弃：那一场没比完，路径上的节点停在算了一半的状
  // 态，再取下去只会给出没有意义的名次。用一个闩把这条不变量钉死，而不
  // 是只写在注释里指望调用方守规矩。
  bool aborted_ = false;
};

}  // namespace detail

}  // namespace pzt::core::tournament
