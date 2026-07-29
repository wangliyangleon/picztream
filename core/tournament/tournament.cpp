#include "core/tournament/tournament.h"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <utility>

#include "core/media/media.h"
#include "core/tagging/tagging.h"

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

// 簇内单淘汰锦标赛。members 是簇内全部成员(size>=2)，两两 compare_fn 比
// 较、奇数个时最后一个轮空直接晋级，直到只剩一个。任意一步解码失败或
// compare_fn 返回 Err 都视为"这一簇 AI 失败"，返回 nullopt 让调用方退化
// 成 keep_id，不中断其它簇。N 个成员恰好 N-1 次比较，不管轮空怎么分布。
//
// on_comparison_start 在每次 compare_fn 之前调一次，参数是这一簇内的第几
// 次比较(1-based)。簇内比较是串行网络调用、每次可能几十秒，没有这个钩子
// 的话调用方最细只能报到簇粒度，大簇期间画面完全静止。轮空不算一次比
// 较——它不发请求，报了会让计数虚高、对不上 AiGateFn 给用户看的总数。
//
// 返回 false = 别比了，直接收手(返回 nullopt)。这是取消唯一能插进来的地
// 方：比较边界。用返回值而不是再加一个 CancelFn 参数，是因为"在每次比较
// 之前"这个时机两者完全一样，多一个参数只会多一处要保持同步的调用点。
// 调用方靠自己那份 CancelFn(粘性的)区分收到的 nullopt 是"取消"还是"AI 失
// 败要退化"——run_bracket 自己不需要知道这个区别。
using ComparisonStartFn = std::function<bool(int index_in_cluster)>;

std::optional<project::ImageId> run_bracket(db::Database& db, const std::string& root_path,
                                             const std::vector<project::ImageId>& members,
                                             ai::Provider provider, const ai::LocalModelConfig& local_config,
                                             const dedup::detail::PreviewDecodeFn& decode_fn,
                                             const detail::CompareFn& compare_fn,
                                             const ComparisonStartFn& on_comparison_start = nullptr) {
  struct Contestant {
    project::ImageId id;
    decode::DecodedImage image;
  };

  std::vector<Contestant> round;
  round.reserve(members.size());
  for (auto id : members) {
    auto img = decode_member(db, root_path, id, decode_fn);
    if (!img) return std::nullopt;
    round.push_back(Contestant{id, std::move(*img)});
  }

  int comparisons_done = 0;
  while (round.size() > 1) {
    std::vector<Contestant> next_round;
    next_round.reserve((round.size() + 1) / 2);
    for (std::size_t i = 0; i < round.size(); i += 2) {
      if (i + 1 < round.size()) {
        if (on_comparison_start && !on_comparison_start(++comparisons_done)) return std::nullopt;
        auto result = compare_fn(round[i].image, round[i + 1].image, provider, local_config);
        if (!result.ok()) return std::nullopt;
        std::size_t winner_idx = result.value().winner == 0 ? i : i + 1;
        next_round.push_back(std::move(round[winner_idx]));
      } else {
        next_round.push_back(std::move(round[i]));  // 奇数个，轮空直接晋级
      }
    }
    round = std::move(next_round);
  }
  return round.front().id;
}

}  // namespace

namespace detail {

Result<ChooseSummary, project::ProjectNotFoundError> cluster_and_choose_impl(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold, const std::vector<std::string>& exclude_tag_names,
    bool apply_dup_tag, bool ai_enabled, ai::Provider ai_provider, const ai::LocalModelConfig& local_config,
    dedup::detail::PreviewDecodeFn decode_fn, CompareFn compare_fn, dedup::DedupProgressFn on_progress,
    AiGateFn on_ai_gate, AiProgressFn on_ai_progress, CancelFn on_cancel) {
  auto project_summary = project::open_project(db, project_id);
  if (!project_summary.ok()) {
    return Result<ChooseSummary, project::ProjectNotFoundError>::Err(project_summary.error());
  }
  const std::string& root_path = project_summary.value().root_path;

  // 排除集合：泛化 dedup/curate 今天各自硬编码的单标签排除(废片 / 废片+
  // 重复)成一个标签名列表，标签不存在时按"不排除任何东西"处理。
  std::unordered_set<project::ImageId> excluded;
  for (const auto& tag_name : exclude_tag_names) {
    if (auto tag_id = tagging::find_tag_by_name(db, project_id, tag_name)) {
      auto tagged = tagging::images_with_tag(db, image_ids, *tag_id);
      excluded.insert(tagged.begin(), tagged.end());
    }
  }
  std::vector<project::ImageId> candidates;
  candidates.reserve(image_ids.size());
  for (auto id : image_ids) {
    if (!excluded.count(id)) candidates.push_back(id);
  }

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
  if (ai_enabled && on_ai_gate && !groups.empty()) {
    if (!on_ai_gate(static_cast<int>(groups.size()), comparison_total)) {
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
      auto ai_winner = run_bracket(db, root_path, g.image_ids, ai_provider, local_config, decode_fn,
                                    compare_fn, on_comparison_start);
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
  return detail::cluster_and_choose_impl(db, project_id, image_ids, time_window_seconds, hash_threshold,
                                          exclude_tag_names, apply_dup_tag, ai_enabled, ai_provider,
                                          local_config, media::decode_preview_file, ai::request_comparison,
                                          std::move(on_progress), std::move(on_ai_gate),
                                          std::move(on_ai_progress), std::move(on_cancel));
}

}  // namespace pzt::core::tournament
