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
std::optional<project::ImageId> run_bracket(db::Database& db, const std::string& root_path,
                                             const std::vector<project::ImageId>& members,
                                             ai::Provider provider, const ai::LocalModelConfig& local_config,
                                             const dedup::detail::PreviewDecodeFn& decode_fn,
                                             const detail::CompareFn& compare_fn) {
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

  while (round.size() > 1) {
    std::vector<Contestant> next_round;
    next_round.reserve((round.size() + 1) / 2);
    for (std::size_t i = 0; i < round.size(); i += 2) {
      if (i + 1 < round.size()) {
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
    dedup::detail::PreviewDecodeFn decode_fn, CompareFn compare_fn, dedup::DedupProgressFn on_progress) {
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

  auto groups = dedup::detail::find_duplicates_impl(db, root_path, candidates, time_window_seconds,
                                                      hash_threshold, on_progress, decode_fn);

  std::unordered_set<project::ImageId> grouped_ids;
  for (const auto& g : groups) grouped_ids.insert(g.image_ids.begin(), g.image_ids.end());

  std::vector<ClusterChoice> clusters;
  clusters.reserve(groups.size() + candidates.size());
  int ai_fallback_count = 0;

  for (const auto& g : groups) {
    project::ImageId winner = g.keep_id;  // AI 关时的答案，AI 开且成功时会被覆盖
    if (ai_enabled) {
      auto ai_winner =
          run_bracket(db, root_path, g.image_ids, ai_provider, local_config, decode_fn, compare_fn);
      if (ai_winner) {
        winner = *ai_winner;
      } else {
        ++ai_fallback_count;  // 退化：winner 维持 g.keep_id
      }
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
    dedup::DedupProgressFn on_progress) {
  return detail::cluster_and_choose_impl(db, project_id, image_ids, time_window_seconds, hash_threshold,
                                          exclude_tag_names, apply_dup_tag, ai_enabled, ai_provider,
                                          local_config, media::decode_preview_file, ai::request_comparison,
                                          std::move(on_progress));
}

}  // namespace pzt::core::tournament
