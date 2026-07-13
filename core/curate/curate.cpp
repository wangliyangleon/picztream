#include "core/curate/curate.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>

#include "core/ai/evaluation.h"
#include "core/browse/browse.h"
#include "core/dedup/dedup.h"

namespace pzt::core::curate {

namespace {

// 候选集：passes_gate 为真 且 非"废片" 且 非"重复"（复用 get_image 的
// evaluation/passes_gate、tagging::images_with_tag）。未评估的图不进候
// 选。标签本身不存在时(项目里从没跑过废片/重复标记)，那一类排除为空，
// 不当错误处理。
std::vector<project::ImageId> resolve_candidates(db::Database& db, project::ProjectId project_id,
                                                   std::optional<tagging::TagId> candidate_scope) {
  std::vector<project::ImageId> ids;
  if (candidate_scope) {
    auto filtered = browse::filter_by_tag(db, *candidate_scope);
    if (filtered.ok()) {
      for (const auto& ref : filtered.value()) ids.push_back(ref.id);
    }
  } else {
    for (const auto& ref : browse::list_images(db, project_id)) ids.push_back(ref.id);
  }

  std::unordered_set<project::ImageId> excluded;
  if (auto reject_tag = tagging::find_tag_by_name(db, project_id, tagging::kRejectTagName)) {
    auto tagged = tagging::images_with_tag(db, ids, *reject_tag);
    excluded.insert(tagged.begin(), tagged.end());
  }
  if (auto dup_tag = tagging::find_tag_by_name(db, project_id, tagging::kDuplicateTagName)) {
    auto tagged = tagging::images_with_tag(db, ids, *dup_tag);
    excluded.insert(tagged.begin(), tagged.end());
  }

  std::vector<project::ImageId> candidates;
  for (auto id : ids) {
    if (excluded.count(id)) continue;
    auto info = project::get_image(db, id);
    if (!info || !info->evaluation || !ai::passes_gate(*info->evaluation)) continue;
    candidates.push_back(id);
  }
  return candidates;
}

// 分簇：复用 dedup::find_duplicates 现场重新分簇(不是读历史分组，候选
// 集已经排除了"重复"标签图，没有旧状态可读，见
// docs/M4_Eng_Design.md 第三节)。候选里没有出现在任何 DuplicateGroup
// 里的(包括所有没有 captured_at 的图，find_duplicates 内部会跳过它
// 们)，各自单独成一簇。只需要每簇的代表 id——簇的非代表成员不会进入
// 最终选择(见下面 curate() 里"簇数 < N 不回填同簇候选"的说明)，不用
// 保留完整 membership。
std::vector<project::ImageId> build_cluster_reps(db::Database& db, const std::string& root_path,
                                                   const std::vector<project::ImageId>& candidates,
                                                   int time_window_seconds, int hash_threshold) {
  auto groups = dedup::find_duplicates(db, root_path, candidates, time_window_seconds, hash_threshold);

  std::unordered_set<project::ImageId> grouped;
  std::vector<project::ImageId> reps;
  for (auto& g : groups) {
    for (auto id : g.image_ids) grouped.insert(id);
    reps.push_back(g.keep_id);
  }
  for (auto id : candidates) {
    if (!grouped.count(id)) reps.push_back(id);
  }
  return reps;
}

struct RepInfo {
  project::ImageId id;
  int score;
  std::optional<std::int64_t> captured_at;
};

RepInfo make_rep_info(db::Database& db, project::ImageId id) {
  auto info = project::get_image(db, id);
  return RepInfo{id, ai::overall_score(*info->evaluation), info->captured_at};
}

// 从 pool 里挑一个：分数最高；打平时选"跟已选集时间差最大"的(farthest-
// point：对每个打平候选算它离已选集里每个有 captured_at 的成员的时间
// 差，取最小值，选这个最小值最大的那个)；已选集为空、打平候选都没有
// captured_at、或距离也打平，退化成跟 dedup::pick_keep_id 同一套兜底：
// captured_at 更新优先，再 id 最小。
RepInfo greedy_pick(std::vector<RepInfo>& pool, const std::vector<RepInfo>& selected) {
  int best_score = pool[0].score;
  for (auto& p : pool) best_score = std::max(best_score, p.score);

  std::vector<std::size_t> tied;
  for (std::size_t i = 0; i < pool.size(); ++i) {
    if (pool[i].score == best_score) tied.push_back(i);
  }

  std::size_t chosen = tied[0];
  if (tied.size() > 1) {
    std::vector<std::int64_t> selected_times;
    for (auto& s : selected) {
      if (s.captured_at) selected_times.push_back(*s.captured_at);
    }

    std::optional<std::int64_t> best_distance;
    bool have_time_pick = false;
    for (auto idx : tied) {
      if (!pool[idx].captured_at || selected_times.empty()) continue;
      std::int64_t min_dist = std::numeric_limits<std::int64_t>::max();
      for (auto t : selected_times) {
        min_dist = std::min(min_dist, std::abs(*pool[idx].captured_at - t));
      }
      bool better = !best_distance || min_dist > *best_distance ||
                    (min_dist == *best_distance && pool[idx].id < pool[chosen].id);
      if (better) {
        best_distance = min_dist;
        chosen = idx;
        have_time_pick = true;
      }
    }

    if (!have_time_pick) {
      chosen = tied[0];
      for (auto idx : tied) {
        auto idx_time = pool[idx].captured_at.value_or(std::numeric_limits<std::int64_t>::min());
        auto chosen_time = pool[chosen].captured_at.value_or(std::numeric_limits<std::int64_t>::min());
        bool better = idx_time != chosen_time ? idx_time > chosen_time : pool[idx].id < pool[chosen].id;
        if (better) chosen = idx;
      }
    }
  }

  RepInfo result = pool[chosen];
  pool.erase(pool.begin() + static_cast<long>(chosen));
  return result;
}

}  // namespace

CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold) {
  auto candidates = resolve_candidates(db, project_id, candidate_scope);
  if (candidates.empty()) return CurateResult{{}, count, 0};

  // project_id 由调用方(pzt curate 命令)调用前已经用 resolve_project_json
  // 验证过存在，这里不会失败——跟 core/api.cpp 其它门面对已验证 project_id
  // 的处理一致，不再二次判空。
  auto project_summary = project::open_project(db, project_id);
  auto cluster_reps = build_cluster_reps(db, project_summary.value().root_path, candidates,
                                          time_window_seconds, hash_threshold);

  std::vector<RepInfo> selected_info;
  std::vector<project::ImageId> selected;

  if (static_cast<int>(cluster_reps.size()) >= count) {
    std::vector<RepInfo> pool;
    for (auto id : cluster_reps) pool.push_back(make_rep_info(db, id));
    for (int i = 0; i < count && !pool.empty(); ++i) {
      auto picked = greedy_pick(pool, selected_info);
      selected.push_back(picked.id);
      selected_info.push_back(picked);
    }
  } else {
    // 簇数 < N：只返回每簇一张代表，不回填同簇的非代表成员——同簇成员
    // 本来就是 build_cluster_reps 判定过的"近似重复"，回填会让最终结
    // 果里出现彼此近重复的图，违背 curate 存在的多样性目的(真机验证时
    // 发现的问题：只有 1-2 张候选、互相近似的场景下，回填会把这批近重
    // 复图凑数塞进 N 张里)。宁可 returned < requested，也不用近重复凑
    // 数——不足的部分如实反映在 returned 里，不报错(算法设计第 5 条)。
    std::vector<RepInfo> reps;
    for (auto id : cluster_reps) reps.push_back(make_rep_info(db, id));
    std::sort(reps.begin(), reps.end(), [](const RepInfo& a, const RepInfo& b) {
      if (a.score != b.score) return a.score > b.score;
      auto at = a.captured_at.value_or(std::numeric_limits<std::int64_t>::min());
      auto bt = b.captured_at.value_or(std::numeric_limits<std::int64_t>::min());
      if (at != bt) return at > bt;
      return a.id < b.id;
    });
    for (auto& r : reps) {
      selected.push_back(r.id);
      selected_info.push_back(r);
    }
  }

  return CurateResult{selected, count, static_cast<int>(selected.size())};
}

}  // namespace pzt::core::curate
