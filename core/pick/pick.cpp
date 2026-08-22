#include "core/pick/pick.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "core/media/media.h"
#include "core/scope/scope.h"
#include "core/tagging/tagging.h"
#include "core/tournament/topk.h"

namespace pzt::core::pick {

namespace detail {

namespace {

// 分簇复用 tournament::cluster_and_choose_impl 关 AI 的那条路：它已经按
// "先排除标签、再分簇"的顺序做完了候选筛选与聚类，且关 AI 时一次比较都
// 不发起(每簇的 winner 直接取 dedup 算好的 keep_id，pick 随后会用人工锦
// 标赛把它覆盖掉)。分簇规则因此只有一份 - pick 要的"同一场景"粒度与
// curate 是同一组参数，各写一遍迟早会分出两套不一样的簇。
constexpr const char* kExcludeTags[] = {tagging::kRejectTagName, tagging::kDuplicateTagName};

std::vector<std::string> exclude_tag_names() {
  return std::vector<std::string>(std::begin(kExcludeTags), std::end(kExcludeTags));
}

Result<PickResult, project::ProjectNotFoundError> ok(PickResult result) {
  return Result<PickResult, project::ProjectNotFoundError>::Ok(std::move(result));
}

}  // namespace

Result<PickResult, project::ProjectNotFoundError> pick_impl(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int count, int time_window_seconds, int hash_threshold,
    dedup::detail::PreviewDecodeFn decode_fn, CompareFn compare_fn, PickGateFn on_gate,
    PickProgressFn on_progress, CancelFn on_cancel) {
  auto project_summary = project::open_project(db, project_id);
  if (!project_summary.ok()) {
    return Result<PickResult, project::ProjectNotFoundError>::Err(project_summary.error());
  }
  const std::string& root_path = project_summary.value().root_path;

  // 候选数 C 在分簇**之前**就要知道：候选不足那条短路的承诺是"一次比较
  // 都不发起"，先把整批解码一遍算 dHash 再发现用不上，是白花的开销。排
  // 除规则本身仍然只有 scope::exclude_by_tags 一处，下面 cluster_and_
  // choose_impl 拿到的已经是筛过的这一批，再筛一次是幂等的。
  auto candidates = scope::exclude_by_tags(db, project_id, image_ids, exclude_tag_names());
  int candidate_count = static_cast<int>(candidates.size());

  if (count <= 0 || candidate_count <= count) {
    PickResult insufficient;
    insufficient.cost.candidate_count = candidate_count;
    insufficient.insufficient_candidates = true;
    return ok(std::move(insufficient));
  }

  auto cancelled_result = [&](int comparisons_done) {
    PickResult cancelled;
    cancelled.comparisons_done = comparisons_done;
    cancelled.cancelled = true;
    return cancelled;
  };

  auto summary = tournament::detail::cluster_and_choose_impl(
      db, project_id, candidates, time_window_seconds, hash_threshold, exclude_tag_names(),
      /*apply_dup_tag=*/false, /*ai_enabled=*/false, decode_fn, compare_fn, /*on_progress=*/nullptr,
      /*on_ai_gate=*/nullptr, /*on_ai_progress=*/nullptr, on_cancel);
  if (!summary.ok()) {
    return Result<PickResult, project::ProjectNotFoundError>::Err(summary.error());
  }
  if (summary.value().cancelled) return ok(cancelled_result(0));

  const auto& clusters = summary.value().clusters;
  int champion_count = static_cast<int>(clusters.size());
  int selected_count = std::min(count, champion_count);

  PickCost cost;
  cost.candidate_count = candidate_count;
  cost.champion_count = champion_count;
  cost.first_stage_comparisons = candidate_count - champion_count;
  // N >= m 时第二级整个不跑(不补位)，所以那条路上的上界就是第一级的精确
  // 值 - 报一个包含第二级的数字，进度条会停在一个到不了的刻度上。
  cost.max_comparisons =
      cost.first_stage_comparisons +
      (count >= champion_count ? 0 : tournament::max_comparisons_for_top_k(champion_count, count));
  cost.reject_count = candidate_count - selected_count;

  if (on_gate && !on_gate(cost)) {
    PickResult declined;
    declined.cost = cost;
    declined.declined = true;
    return ok(std::move(declined));
  }

  // 两级共用的一场比较的开头：先查取消(取消之后这一场不会发生，报一个
  // "正在比较第 N 次"只会让最后停住的那一帧多走一格)，再把这一场的两级
  // 计数报出去。返回 false 就是整场收手，与比较原语返回 nullopt 走同一
  // 条出口 - 人在环这条路上没有"AI 失败"那种第三种可能，两者都是"这次
  // 什么都没发生"。
  int comparisons_done = 0;
  PickProgress progress;
  progress.max_comparisons = cost.max_comparisons;
  auto match_start = [&](PickStage stage) {
    return [&, stage](int rank_index, int match_index) {
      if (on_cancel && on_cancel()) return false;
      ++comparisons_done;
      if (!on_progress) return true;
      progress.stage = stage;
      progress.comparisons_done = comparisons_done;
      if (stage == PickStage::Cluster) {
        progress.match_index = match_index;
      } else {
        progress.rank_index = rank_index;
      }
      on_progress(progress);
      return true;
    };
  };

  // 第一级：簇内决冠军。单例(size == 1)直接进第二级，select_top_k 对
  // 单个成员零比较，但连查一次库都省了。
  std::vector<project::ImageId> champions;
  champions.reserve(clusters.size());
  progress.group_total = 0;
  for (const auto& cluster : clusters) {
    if (cluster.members.size() >= 2) ++progress.group_total;
  }
  progress.group_index = 0;
  for (const auto& cluster : clusters) {
    if (cluster.members.size() < 2) {
      champions.push_back(cluster.winner);
      continue;
    }
    ++progress.group_index;
    progress.match_total = static_cast<int>(cluster.members.size()) - 1;
    auto champion = tournament::select_top_k(db, root_path, cluster.members, 1, decode_fn, compare_fn,
                                              match_start(PickStage::Cluster));
    if (champion.aborted || champion.ranked.empty()) return ok(cancelled_result(comparisons_done));
    champions.push_back(champion.ranked.front());
  }

  std::vector<project::ImageId> selected;
  if (count >= champion_count) {
    selected = champions;
  } else {
    progress.rank_total = selected_count;
    auto ranked = tournament::select_top_k(db, root_path, champions, count, decode_fn, compare_fn,
                                            match_start(PickStage::Final));
    if (ranked.aborted) return ok(cancelled_result(comparisons_done));
    selected = std::move(ranked.ranked);
  }

  // 写库统一在这最后一步，所以上面每一条提前返回都天然是零写入，不需要
  // 任何回滚。最后再查一次取消：粘性的 CancelFn 让"最后一场比完之后才喊
  // 停"也来得及。
  if (on_cancel && on_cancel()) return ok(cancelled_result(comparisons_done));

  std::unordered_set<project::ImageId> selected_set(selected.begin(), selected.end());
  std::vector<project::ImageId> rejected;
  rejected.reserve(candidates.size() - selected_set.size());
  for (auto id : candidates) {
    if (!selected_set.count(id)) rejected.push_back(id);
  }

  PickResult result;
  result.selected = std::move(selected);
  result.cost = cost;
  result.comparisons_done = comparisons_done;
  // 入选的不打任何标签："被选中"的表示就是"没有废片标签"。
  auto tagged = tagging::add_tag_to_images(db, rejected, tagging::ensure_reject_tag(db, project_id));
  result.rejected_count = tagged.ok() ? tagged.value() : 0;
  return ok(std::move(result));
}

}  // namespace detail

Result<PickResult, project::ProjectNotFoundError> pick(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int count, int time_window_seconds, int hash_threshold, CompareFn compare_fn, PickGateFn on_gate,
    PickProgressFn on_progress, CancelFn on_cancel) {
  return detail::pick_impl(db, project_id, image_ids, count, time_window_seconds, hash_threshold,
                            media::decode_preview_file, std::move(compare_fn), std::move(on_gate),
                            std::move(on_progress), std::move(on_cancel));
}

}  // namespace pzt::core::pick
