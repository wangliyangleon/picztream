#include "core/tournament/tournament.h"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <utility>

#include "core/ai/compare.h"
#include "core/media/media.h"
#include "core/scope/scope.h"
#include "core/tagging/tagging.h"
#include "core/tournament/topk.h"

namespace pzt::core::tournament {

namespace {

// 给一个候选成员取它的解码结果，供 AI 比较用——find_duplicates_impl 内部
// 已经为算 dHash 解码过一次，但那次解码结果是它的内部细节没有透传出来；
// 这里对"落进 size>=2 簇、且 AI 开"的这一小部分图片重新解码一次，不是全
// 量重复解码。project::get_image 拿 file_path/kind/preview_cache_path，
// 配合 media::resolve_preview_path 拼出实际要传给 decode_fn 的路径。
std::optional<decode::DecodedImage> decode_member(db::Database& db, const std::string& root_path,
                                                    project::ImageId id,
                                                    const dedup::detail::PreviewDecodeFn& decode_fn) {
  auto info = project::get_image(db, id);
  if (!info) return std::nullopt;
  std::string path =
      media::resolve_preview_path(root_path, info->file_path, info->kind, info->preview_cache_path);
  auto decoded = decode_fn(path);
  if (!decoded.ok()) return std::nullopt;
  return decoded.value();
}

// 簇内单淘汰锦标赛。members 是簇内全部成员(size>=2)，建一棵 LoserTree、
// 取第 1 名就是簇冠军。配对规则(哪个位置跟哪个位置比、轮空落在哪)收在
// LoserTree 里，这一层不再自己写一遍 - CompareFn 的契约里没有传递性(A 赢
// B、B 赢 C，不代表 A 赢 C)，换一种配对方式可能真的换出一个不同的赢家，
// 所以配对规则只能有一份，两处各写一遍迟早会换出两个不同的答案。N 个成
// 员恰好 N-1 次比较，不管轮空怎么分布(建树把每个内部节点比一遍，而内部
// 节点恰好 N-1 个)。比较由谁做出来这一层不知道，也不需要知道(见
// tournament.h 上 CompareFn 的说明)。
//
// 任意一步解码失败或 compare_fn 返回 nullopt 都视为"这一簇比较失败"，返
// 回 nullopt 让调用方退化成 keep_id，不中断其它簇。这一条与 topk.h 的
// select_top_k **有意不同**：那边是人在环，整簇退化意味着用户已经按过的
// 比较全部白按、还会看到一个自己没选过的结果进决赛，所以那边让解码失败
// 的一方判负；AI 这条路上退化不产生用户可感知的损失。
//
// 解码是逐对惰性做的，不是开赛前把整簇一次性解码完 - 跟 dedup.cpp::
// find_duplicates_impl 逐张解码、用完就让局部变量出作用域释放是同一个模
// 式(T-28)。树里全程只存成员下标、不存解码结果，一对成员只在真的要送进
// compare_fn 前才解码，解码结果是 lambda 体内的局部变量，比较完这对就跟
// 着出作用域释放，峰值内存从 O(簇大小)降到常数(同一时刻最多两张解码图片
// 存活，不管簇有多大)。代价是晋级的赢家在后面几轮会被重新解码：每次比较
// 固定解码 2 张，N 个成员共 N-1 次比较，总解码次数恒为 2*(N-1)，相对"每
// 张只解一次"的下限最多多 2 倍且不随簇大小继续变大(1.6x@N=5、1.9x@N=32、
// 2.0x@N→∞) - 不是 O(簇大小×log 簇大小) 那种会随簇变大而变大的开销。
//
// 另一个代价：解码跟比较交替进行，靠后的成员解码失败时，前面几轮的真实
// AI 比较已经发生过、结果被这次退化整个扔掉。终态不变(仍然是这一簇整体
// 退化成 keep_id)，变的只是"退化前已经花掉的网络调用数"这个不影响正确性
// 的成本细节。
//
// on_comparison_start 在每次 compare_fn 之前(且在这对的解码之前)调一次，
// 参数是这一簇内的第几次比较(1-based)。簇内比较是串行网络调用、每次可能
// 几十秒，没有这个钩子的话调用方最细只能报到簇粒度，大簇期间画面完全静
// 止。轮空不算一次比较 - 它不发请求，报了会让计数虚高、对不上 AiGateFn 给
// 用户看的总数(LoserTree 对轮空根本不建节点，所以这里天然不会被调到)。放
// 在解码之前调用是为了让挂在这个钩子里的取消检查能在解码这对成员的开销发
// 生之前生效，跟 dedup.cpp"取消检查放在解码之前"同一个理由。
//
// 返回 false = 别比了，直接收手(返回 nullopt)。这是取消唯一能插进来的地
// 方：比较边界。用返回值而不是再加一个 CancelFn 参数，是因为"在每次比较
// 之前"这个时机两者完全一样，多一个参数只会多一处要保持同步的调用点。
// 调用方靠自己那份 CancelFn(粘性的)区分收到的 nullopt 是"取消"还是"AI 失
// 败要退化" - run_bracket 自己不需要知道这个区别。
using ComparisonStartFn = std::function<bool(int index_in_cluster)>;

std::optional<project::ImageId> run_bracket(db::Database& db, const std::string& root_path,
                                             const std::vector<project::ImageId>& members,
                                             const dedup::detail::PreviewDecodeFn& decode_fn,
                                             const detail::CompareFn& compare_fn,
                                             const ComparisonStartFn& on_comparison_start = nullptr) {
  int comparisons_done = 0;
  detail::IndexCompareFn compare = [&](int left, int right) -> std::optional<detail::ComparisonWinner> {
    if (on_comparison_start && !on_comparison_start(++comparisons_done)) return std::nullopt;
    auto left_image = decode_member(db, root_path, members[static_cast<std::size_t>(left)], decode_fn);
    if (!left_image) return std::nullopt;
    auto right_image = decode_member(db, root_path, members[static_cast<std::size_t>(right)], decode_fn);
    if (!right_image) return std::nullopt;
    return compare_fn(*left_image, *right_image);
  };

  detail::LoserTree tree(static_cast<int>(members.size()));
  auto champion = tree.extract_next(compare);
  if (champion.status != detail::ExtractStatus::Ok) return std::nullopt;
  return members[static_cast<std::size_t>(champion.member)];
}

}  // namespace

namespace detail {

Result<ChooseSummary, project::ProjectNotFoundError> cluster_and_choose_impl(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold, const std::vector<std::string>& exclude_tag_names,
    bool apply_dup_tag, bool ai_enabled, dedup::detail::PreviewDecodeFn decode_fn, CompareFn compare_fn,
    dedup::DedupProgressFn on_progress, AiGateFn on_ai_gate, AiProgressFn on_ai_progress,
    CancelFn on_cancel) {
  auto project_summary = project::open_project(db, project_id);
  if (!project_summary.ok()) {
    return Result<ChooseSummary, project::ProjectNotFoundError>::Err(project_summary.error());
  }
  const std::string& root_path = project_summary.value().root_path;

  // 排除集合：dedup 传 {废片}、curate 传 {废片,重复}，标签不存在时按"不排
  // 除任何东西"处理。规则本身收在 core::scope(T-16)，这里只负责把调用方给
  // 的标签名列表递过去。不传 scope_tag —— 锦标赛这条路上没有 F-26 的对称
  // 例外：范围本身是废片时 dedup 直接拒绝(#27 决策 D-2)，不是"照做但不排"。
  auto candidates = scope::exclude_by_tags(db, project_id, image_ids, exclude_tag_names);

  if (candidates.empty()) {
    return Result<ChooseSummary, project::ProjectNotFoundError>::Ok(ChooseSummary{{}, 0, 0, 0});
  }

  int skipped_no_capture_time =
      static_cast<int>(candidates.size()) -
      static_cast<int>(dedup::images_with_capture_time(db, candidates).size());

  // 取消结果的构造只有这一处：clusters 空、tagged_count 0，跟 ai_declined
  // 的空结果同形，区别只在哪个 flag 为真。
  auto cancelled_summary = [&] {
    ChooseSummary cancelled{};
    cancelled.tagged_count = 0;
    cancelled.skipped_no_capture_time = skipped_no_capture_time;
    cancelled.ai_fallback_count = 0;
    cancelled.cancelled = true;
    return Result<ChooseSummary, project::ProjectNotFoundError>::Ok(std::move(cancelled));
  };

  auto groups = dedup::detail::find_duplicates_impl(db, root_path, candidates, time_window_seconds,
                                                      hash_threshold, on_progress, decode_fn, on_cancel);
  // 分簇阶段被取消时 find_duplicates_impl 提前返回手上那点结果,它没法用
  // 返回值表达"我是被取消的"(返回类型就是一个 vector)。CancelFn 粘性,所
  // 以在这里再查一次就能区分——不带 --ai 时这是唯一的取消出口。
  if (on_cancel && on_cancel()) return cancelled_summary();

  std::unordered_set<project::ImageId> grouped_ids;
  for (const auto& g : groups) grouped_ids.insert(g.image_ids.begin(), g.image_ids.end());

  // 单淘汰赛 N 个成员恰好 N-1 场，所以这是精确值不是估算：闸门拿它问用户
  // "要不要为此发这么多次请求"，进度也拿它当分母——两处必须是同一个数，
  // 否则用户点头时看到的开销和进度条走的刻度对不上。
  int comparison_total = 0;
  for (const auto& g : groups) comparison_total += static_cast<int>(g.image_ids.size()) - 1;

  // 开跑闸门：本地分簇已经跑完(便宜、无副作用)，但一次 request_comparison
  // 都还没发、一个标签都还没写，所以这里是唯一一个"能报出精确开销、且拒绝
  // 之后系统状态跟没执行过完全一样"的位置。放在下面写库那一段之前直接
  // return，取消的原子性就不需要任何回滚逻辑来保证。
  // 候选总数 = 成簇的 + 落单的，正好是下面 clusters 最终的大小。在这里
  // 算而不是等 clusters 造出来，是因为闸门必须问在任何一次比较之前，而
  // curate 要拿这个数算"要评估多少张"(见 dedup.h 上 AiCost 的说明)。
  int candidate_total =
      static_cast<int>(groups.size()) +
      (static_cast<int>(candidates.size()) - static_cast<int>(grouped_ids.size()));

  if (ai_enabled && on_ai_gate && !groups.empty()) {
    if (!on_ai_gate(AiCost{static_cast<int>(groups.size()), comparison_total, candidate_total})) {
      ChooseSummary declined{};
      declined.tagged_count = 0;
      declined.skipped_no_capture_time = skipped_no_capture_time;
      declined.ai_fallback_count = 0;
      declined.ai_declined = true;
      return Result<ChooseSummary, project::ProjectNotFoundError>::Ok(std::move(declined));
    }
  }

  std::vector<ClusterChoice> clusters;
  clusters.reserve(groups.size() + candidates.size());
  int ai_fallback_count = 0;

  int ai_done = 0;
  int comparisons_before_this_cluster = 0;
  for (const auto& g : groups) {
    project::ImageId winner = g.keep_id;  // AI 关时的答案，AI 开且成功时会被覆盖
    if (ai_enabled) {
      ++ai_done;
      // 每次比较发起**之前**报一次(不是之后，也不是每簇一次)——理由见
      // tournament.h 上 AiProgressFn 的说明，两点都是真机上踩出来的。
      // comparison_done 用 comparisons_before_this_cluster 做基数，所以某
      // 簇中途失败、剩下的比较没发生时，进入下一簇会自动补齐，计数不会掉
      // 队到一个永远够不着总数的位置。
      ComparisonStartFn on_comparison_start = nullptr;
      if (on_ai_progress || on_cancel) {
        on_comparison_start = [&](int index_in_cluster) {
          // 先查取消再报进度:取消之后这次比较不会发生,报一个"正在比较第 N
          // 次"只会让最后停住的那一帧多走一格、对不上实际发生的事。
          if (on_cancel && on_cancel()) return false;
          if (on_ai_progress) {
            on_ai_progress(AiProgress{ai_done, static_cast<int>(groups.size()),
                                       comparisons_before_this_cluster + index_in_cluster,
                                       comparison_total});
          }
          return true;
        };
      }
      auto ai_winner =
          run_bracket(db, root_path, g.image_ids, decode_fn, compare_fn, on_comparison_start);
      if (ai_winner) {
        winner = *ai_winner;
      } else if (on_cancel && on_cancel()) {
        // run_bracket 用同一个 nullopt 表达"取消"和"AI 失败"。粘性的
        // CancelFn 让这里能分开:是取消就整条收手(零写入),不是就按既有语
        // 义退化成 keep_id、继续跑下一簇。顺序不能反——先判退化的话，取消
        // 会被记成一次 AI 失败，用户拿到的摘要就成了"某簇比较失败"。
        return cancelled_summary();
      } else {
        ++ai_fallback_count;  // 退化：winner 维持 g.keep_id
      }
      comparisons_before_this_cluster += static_cast<int>(g.image_ids.size()) - 1;
    }
    clusters.push_back(ClusterChoice{g.image_ids, winner});
  }

  std::vector<project::ImageId> singletons;
  for (auto id : candidates) {
    if (!grouped_ids.count(id)) singletons.push_back(id);
  }
  std::sort(singletons.begin(), singletons.end());
  for (auto id : singletons) {
    clusters.push_back(ClusterChoice{{id}, id});
  }

  int tagged_count = 0;
  if (apply_dup_tag) {
    // 先摘光再重新打：清标记作用于完整的 image_ids(不只是 candidates)，
    // 跟今天 dedup::find_and_tag_duplicates 的时机一致，避免排除标签的
    // 图上残留旧的"重复"标记。
    tagging::TagId duplicate_tag_id = tagging::ensure_duplicate_tag(db, project_id);
    for (auto id : image_ids) {
      (void)tagging::remove_tag(db, id, duplicate_tag_id);
    }
    for (const auto& c : clusters) {
      if (c.members.size() < 2) continue;
      for (auto id : c.members) {
        if (id == c.winner) continue;
        if (tagging::add_tag(db, id, duplicate_tag_id).ok()) ++tagged_count;
      }
    }
  }

  return Result<ChooseSummary, project::ProjectNotFoundError>::Ok(
      ChooseSummary{std::move(clusters), tagged_count, skipped_no_capture_time, ai_fallback_count});
}

}  // namespace detail

Result<ChooseSummary, project::ProjectNotFoundError> cluster_and_choose(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold, const std::vector<std::string>& exclude_tag_names,
    bool apply_dup_tag, bool ai_enabled, ai::Provider ai_provider, const ai::LocalModelConfig& local_config,
    dedup::DedupProgressFn on_progress, AiGateFn on_ai_gate, AiProgressFn on_ai_progress,
    CancelFn on_cancel) {
  // AI 那条路的 adapter：把只认"左还是右"的比较原语接到 ai::
  // request_comparison 上。供应商与本地模型配置在这里被捕获，所以
  // bracket 推进那一层拿到的是一个中立的两图比较函数。ComparisonResult
  // 的 reasoning 在这里丢掉 - 锦标赛只需要胜者，理由没有消费者。
  detail::CompareFn compare_fn = [ai_provider, local_config](const decode::DecodedImage& a,
                                                              const decode::DecodedImage& b)
      -> std::optional<detail::ComparisonWinner> {
    auto result = ai::request_comparison(a, b, ai_provider, local_config);
    if (!result.ok()) return std::nullopt;
    return result.value().winner == 0 ? detail::ComparisonWinner::Left : detail::ComparisonWinner::Right;
  };
  return detail::cluster_and_choose_impl(db, project_id, image_ids, time_window_seconds, hash_threshold,
                                          exclude_tag_names, apply_dup_tag, ai_enabled,
                                          media::decode_preview_file, std::move(compare_fn),
                                          std::move(on_progress), std::move(on_ai_gate),
                                          std::move(on_ai_progress), std::move(on_cancel));
}

}  // namespace pzt::core::tournament
