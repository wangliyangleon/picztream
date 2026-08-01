#include "core/curate/curate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>

#include "core/browse/browse.h"
#include "core/tournament/tournament.h"

namespace pzt::core::curate {

namespace {

// 范围解析:candidate_scope 有值走某个标签下的图，否则整个项目。标签排
// 除(废片/重复)不在这里做了——W2026-07-21 目标二收进
// tournament::cluster_and_choose 的 exclude_tag_names，dedup 和 curate
// 现在共用同一份排除逻辑，不再各自维护一份。
std::vector<project::ImageId> resolve_scope_ids(db::Database& db, project::ProjectId project_id,
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
  return ids;
}

struct RepInfo {
  project::ImageId id;
  std::optional<std::int64_t> captured_at;
};

RepInfo make_rep_info(db::Database& db, project::ImageId id) {
  auto info = project::get_image(db, id);
  return RepInfo{id, info->captured_at};
}

// 从 pool 里挑一个：所有代表等价(去分数后无质量维度)，纯 captured_at 多
// 样性。已选集非空时走 farthest-point——对每个候选算它离已选集里每个有
// captured_at 的成员的时间差，取最小值，选这个最小值最大的那个(离已选
// 集整体最远)，打平选 id 最小。已选集为空(seed)、候选都没有 captured_at、
// 或距离也打平，退化成跟 dedup::pick_keep_id 同一套兜底：captured_at 更
// 新优先(seed 取最新)，再 id 最小。
RepInfo greedy_pick(std::vector<RepInfo>& pool, const std::vector<RepInfo>& selected) {
  std::vector<std::int64_t> selected_times;
  for (auto& s : selected) {
    if (s.captured_at) selected_times.push_back(*s.captured_at);
  }

  std::size_t chosen = 0;
  std::optional<std::int64_t> best_distance;
  bool have_time_pick = false;
  for (std::size_t idx = 0; idx < pool.size(); ++idx) {
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
    chosen = 0;
    for (std::size_t idx = 0; idx < pool.size(); ++idx) {
      auto idx_time = pool[idx].captured_at.value_or(std::numeric_limits<std::int64_t>::min());
      auto chosen_time = pool[chosen].captured_at.value_or(std::numeric_limits<std::int64_t>::min());
      bool better = idx_time != chosen_time ? idx_time > chosen_time : pool[idx].id < pool[chosen].id;
      if (better) chosen = idx;
    }
  }

  RepInfo result = pool[chosen];
  pool.erase(pool.begin() + static_cast<long>(chosen));
  return result;
}

}  // namespace

namespace detail {

int preselect_size(int candidate_count, double multiplier, int count) {
  if (candidate_count <= 0 || count <= 0) return 0;
  // double 里算再跟候选集大小取小：count 大到 int 溢出的量级时，先乘再
  // 转 int 是未定义行为，先比较后转换不是。
  double target = std::ceil(std::max(1.5, multiplier) * static_cast<double>(count));
  if (target >= static_cast<double>(candidate_count)) return candidate_count;
  return static_cast<int>(target);
}

}  // namespace detail

CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold, double preselect_multiplier,
                     bool ai_enabled, ai::Provider ai_provider,
                     const ai::LocalModelConfig& local_config, dedup::DedupProgressFn on_progress,
                     dedup::AiProgressFn on_ai_progress) {
  auto ids = resolve_scope_ids(db, project_id, candidate_scope);

  // 分簇 + 每簇选 winner 整个委托给 tournament::cluster_and_choose：排除
  // 废片和重复标签(curate 独有，dedup 只排废片)、apply_dup_tag=false(簇
  // 内落选不等于"重复"，语义不一样，见 curate.h 的说明)。ai_enabled=false
  // 时每簇 winner 就是 find_duplicates 算好的 keep_id，等价于这个函数改
  // 造前的 build_cluster_reps 输出——project_id 由调用方(pzt curate 命
  // 令)调用前已经用 resolve_project_json 验证过存在，这里不会失败，跟
  // core/api.cpp 其它门面对已验证 project_id 的处理一致，不再二次判空。
  auto choose_result = tournament::cluster_and_choose(
      db, project_id, ids, time_window_seconds, hash_threshold,
      {tagging::kRejectTagName, tagging::kDuplicateTagName}, /*apply_dup_tag=*/false, ai_enabled,
      ai_provider, local_config, std::move(on_progress), /*on_ai_gate=*/nullptr,
      std::move(on_ai_progress));
  const auto& summary = choose_result.value();

  if (summary.clusters.empty()) return CurateResult{{}, count, 0, 0};

  std::vector<project::ImageId> winners;
  winners.reserve(summary.clusters.size());
  for (const auto& c : summary.clusters) winners.push_back(c.winner);

  std::vector<project::ImageId> selected;

  if (static_cast<int>(winners.size()) >= count) {
    // 票 04：先把候选集(每簇一张代表)按时间多样性裁成预选集，两条路都走
    // 这一刀。裁剪用的就是下面 AI 关那条路的 farthest-point，所以 AI 关
    // 时输出一字不变——贪心是增量的，"先挑 K 张再从 K 张里挑 count 张"
    // 每一步的 argmax 都落在 K 里，跟直接挑 count 张同序。AI 开时这一刀
    // 是实打实的收窄：随机采样只在预选集里发生。K == winners.size() 时
    // 是空操作(候选集介于 count 与目标之间就是这种情况)。
    int k = detail::preselect_size(static_cast<int>(winners.size()), preselect_multiplier, count);
    if (k < static_cast<int>(winners.size())) {
      std::vector<RepInfo> pool;
      for (auto id : winners) pool.push_back(make_rep_info(db, id));
      std::vector<RepInfo> preselected;
      for (int i = 0; i < k && !pool.empty(); ++i) {
        preselected.push_back(greedy_pick(pool, preselected));
      }
      winners.clear();
      for (const auto& r : preselected) winners.push_back(r.id);
    }

    if (ai_enabled) {
      // AI 开：从 winner 集合随机挑 count 个(PRD 已拍板接受不可复现，见
      // curate.h 的说明)——没有质量分可比，"哪个 winner 更该被选中"本来
      // 就没有确定性答案。
      std::mt19937 rng(std::random_device{}());
      std::sample(winners.begin(), winners.end(), std::back_inserter(selected), count, rng);
    } else {
      // AI 关：farthest-point 多样性，逻辑不变，只是输入源从旧
      // build_cluster_reps 换成 winners(ai_enabled=false 时两者等价)，票
      // 04 之后 winners 可能已经被裁剪过。
      std::vector<RepInfo> pool;
      for (auto id : winners) pool.push_back(make_rep_info(db, id));
      std::vector<RepInfo> selected_info;
      for (int i = 0; i < count && !pool.empty(); ++i) {
        auto picked = greedy_pick(pool, selected_info);
        selected.push_back(picked.id);
        selected_info.push_back(picked);
      }
    }
  } else {
    // 簇数 < N：两种模式都返回全部 winner，不分 ai_enabled——没有"选"这
    // 个动作，谈不上随机还是多样性，统一按 captured_at 降序、id 升序排
    // 序(确定性)。不回填同簇的非 winner 成员，理由同旧版本：同簇成员是
    // "近似重复"或"同一场景"，回填会让最终结果出现彼此近重复的图，违背
    // curate 存在的多样性目的。
    std::vector<RepInfo> reps;
    for (auto id : winners) reps.push_back(make_rep_info(db, id));
    std::sort(reps.begin(), reps.end(), [](const RepInfo& a, const RepInfo& b) {
      auto at = a.captured_at.value_or(std::numeric_limits<std::int64_t>::min());
      auto bt = b.captured_at.value_or(std::numeric_limits<std::int64_t>::min());
      if (at != bt) return at > bt;  // captured_at 降序
      return a.id < b.id;
    });
    for (auto& r : reps) selected.push_back(r.id);
  }

  return CurateResult{selected, count, static_cast<int>(selected.size()), summary.ai_fallback_count};
}

}  // namespace pzt::core::curate
