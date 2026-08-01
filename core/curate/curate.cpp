#include "core/curate/curate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>

#include "core/ai/evaluation.h"
#include "core/ai/evaluation_store.h"
#include "core/browse/browse.h"
#include "core/media/media.h"
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

// 交付顺序：captured_at 降序，打平用 id 升序兜底。没有 captured_at 的取
// int64 最小值，于是稳定落在最后，不引入不确定性。
//
// 两条**确定性**路径（关 AI 凑够 count、以及候选簇数不足 count）共用这一
// 个比较器，是刻意的：两者方向必须一致，否则同一次 curate 会因为候选够
// 不够而交出方向相反的两种排列。提成具名函数而不是各写一遍 lambda，是为
// 了让这个约束是结构上的而不是靠约定。
//
// 开 AI 且候选够那条路径不在此列——它走 std::sample，顺序是簇的遍历顺序，
// 本票不动（票 06 起改由模型决定，见 docs/Intent_Curation_PRD.md 决策十四）。
bool by_captured_at_desc(const RepInfo& a, const RepInfo& b) {
  auto at = a.captured_at.value_or(std::numeric_limits<std::int64_t>::min());
  auto bt = b.captured_at.value_or(std::numeric_limits<std::int64_t>::min());
  if (at != bt) return at > bt;
  return a.id < b.id;
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

// 从 pool 里按 farthest-point 依次取 n 张，取走的从 pool 里移除。裁预选
// 集和最终选片是同一个动作、只是 n 不同(票 04)，共用这一个循环。
std::vector<RepInfo> take_farthest_points(std::vector<RepInfo>& pool, int n) {
  std::vector<RepInfo> taken;
  for (int i = 0; i < n && !pool.empty(); ++i) taken.push_back(greedy_pick(pool, taken));
  return taken;
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

CurateResult detail::curate_impl(db::Database& db, project::ProjectId project_id,
                                  std::optional<tagging::TagId> candidate_scope, int count,
                                  int time_window_seconds, int hash_threshold,
                                  double preselect_multiplier, bool ai_enabled,
                                  ai::Provider ai_provider, const ai::LocalModelConfig& local_config,
                                  detail::EvaluateFn evaluate_fn, dedup::DedupProgressFn on_progress,
                                  dedup::AiProgressFn on_ai_progress, CurateAiGateFn on_ai_gate,
                                  EvalProgressFn on_eval_progress, dedup::CancelFn on_cancel) {
  auto ids = resolve_scope_ids(db, project_id, candidate_scope);

  // 空结果的三种含义各有一个构造点，刻意不共用一个"空"-它们对用户说的
  // 话完全不同(PRD 决策十九)。
  auto declined_result = [&] {
    CurateResult r{{}, count, 0, 0};
    r.ai_declined = true;
    return r;
  };
  auto cancelled_result = [&] {
    CurateResult r{{}, count, 0, 0};
    r.cancelled = true;
    return r;
  };

  // 闸门要报的评估张数 = 预选集大小。候选不足 count 时没有"选"这个动作、
  // 也就没有预选集，评估开销为 0(票 04 定的"裁剪不参与"在开销上的对应)。
  auto evaluation_count_for = [&](int candidate_count) {
    if (candidate_count < count) return 0;
    return detail::preselect_size(candidate_count, preselect_multiplier, count);
  };

  // 分簇 + 每簇选 winner 整个委托给 tournament::cluster_and_choose：排除
  // 废片和重复标签(curate 独有，dedup 只排废片)、apply_dup_tag=false(簇
  // 内落选不等于"重复"，语义不一样，见 curate.h 的说明)。ai_enabled=false
  // 时每簇 winner 就是 find_duplicates 算好的 keep_id，等价于这个函数改
  // 造前的 build_cluster_reps 输出——project_id 由调用方(pzt curate 命
  // 令)调用前已经用 resolve_project_json 验证过存在，这里不会失败，跟
  // core/api.cpp 其它门面对已验证 project_id 的处理一致，不再二次判空。
  // 闸门转接（票 05）：报给调用方的是**合并**开销(比较多少次 + 评估多少
  // 张)，只问一次。tournament 在本地分簇跑完、任何一次比较发出之前调这个
  // lambda，那一刻 AiCost.candidate_count 已经是准的，正好够算评估张数。
  //
  // gate_consulted 记下这一趟到底问没问：tournament 只在存在 size>=2 的簇
  // 时才问(对它自己而言没有比较就没有开销)，而 curate 即使一次比较都不发
  // 也仍然要评估。那种情况下由下面的评估段补问，两处合起来保证"任何视觉
  // 调用之前必然问过一次，且只问一次"。
  bool gate_consulted = false;
  tournament::AiGateFn tournament_gate = nullptr;
  if (ai_enabled && on_ai_gate) {
    tournament_gate = [&](const tournament::AiCost& cost) {
      gate_consulted = true;
      return on_ai_gate(cost.comparison_count, evaluation_count_for(cost.candidate_count));
    };
  }

  auto choose_result = tournament::cluster_and_choose(
      db, project_id, ids, time_window_seconds, hash_threshold,
      {tagging::kRejectTagName, tagging::kDuplicateTagName}, /*apply_dup_tag=*/false, ai_enabled,
      ai_provider, local_config, std::move(on_progress), std::move(tournament_gate),
      std::move(on_ai_progress), on_cancel);
  const auto& summary = choose_result.value();

  // 这三个返回点此前折叠成同一个值(空 selected + returned 0)。前两个必须
  // 先判：ai_declined/cancelled 的结果里 clusters 也是空的，落到下面那句
  // 会被当成"这个项目里一张照片都没有"。
  if (summary.ai_declined) return declined_result();
  if (summary.cancelled) return cancelled_result();
  if (summary.clusters.empty()) return CurateResult{{}, count, 0, 0};

  std::vector<project::ImageId> winners;
  winners.reserve(summary.clusters.size());
  for (const auto& c : summary.clusters) winners.push_back(c.winner);

  std::vector<project::ImageId> selected;

  if (static_cast<int>(winners.size()) >= count) {
    // 票 04：先把候选集(每簇一张代表)按时间多样性裁成预选集，两条路都走
    // 这一刀。裁剪用的就是 AI 关那条路的 farthest-point，所以 AI 关时选
    // 中的仍是同一批 - 贪心是增量的，"先挑 K 张再从 K 张里挑 count 张"
    // 每一步的 argmax 都落在 K 里，跟直接挑 count 张选出同一个集合;顺序
    // 又由票 01 的 by_captured_at_desc 统一定，于是这条路上裁剪前后的输
    // 出一字不变。AI 开时这一刀是实打实的收窄：随机采样只在预选集里发
    // 生。K == winners.size() 时是空操作(候选集介于 count 与目标之间就
    // 是这种情况)。
    int k = detail::preselect_size(static_cast<int>(winners.size()), preselect_multiplier, count);
    bool clamps = k < static_cast<int>(winners.size());

    // 每张代表只读一次库：裁剪与 AI 关那条路共用同一个 pool。AI 开且不
    // 裁剪时一次都不读(这条路只需要 id)，跟票 04 之前一样。
    std::vector<RepInfo> pool;
    if (clamps || !ai_enabled) {
      for (auto id : winners) pool.push_back(make_rep_info(db, id));
    }
    if (clamps) {
      auto preselected = take_farthest_points(pool, k);
      pool = std::move(preselected);
      winners.clear();
      for (const auto& r : pool) winners.push_back(r.id);
    }

    if (ai_enabled) {
      // 只评估预选集(PRD 决策十)：评估次数因此由构造保证有界，与图库大
      // 小无关。已经有评估记录的跳过 - 缓存判据是"有记录就跳过"，不做字
      // 段完整性检查也不加版本号(PRD 决策七)。
      //
      // 算在闸门之前，是为了让补问的那一路能报**精确**张数而不是上界：这
      // 里 winners 已经是预选集本身，扣掉命中缓存的就是真正要发的请求数。
      std::vector<project::ImageId> to_evaluate;
      auto already_evaluated = project::evaluated_image_ids(db, winners);
      for (auto id : winners) {
        if (!already_evaluated.count(id)) to_evaluate.push_back(id);
      }
      int eval_total = static_cast<int>(to_evaluate.size());

      // 票 05：补问闸门。走到这里说明 tournament 没问过 - 它只在存在
      // size>=2 的簇时才问，而全是单例簇的项目一次比较都不发、开销却不为
      // 零(评估还在后面)。此刻仍然满足"任何视觉调用之前"：没有比较发生
      // 过，评估也还没开始。
      //
      // 这一路报的是精确张数。tournament 那一路只能报上界(闸门问在分簇之
      // 后、锦标赛之前，那时还不知道 winner 是谁，无从查缓存)，两者都不
      // 会低报，方向上一致。
      if (on_ai_gate && !gate_consulted && eval_total > 0) {
        gate_consulted = true;
        if (!on_ai_gate(/*comparison_count=*/0, eval_total)) return declined_result();
      }

      for (int i = 0; i < eval_total; ++i) {
        // 先查取消再报进度：取消之后这张不会被评估，报一个"正在评估第 N
        // 张"只会让最后停住的那一帧多走一格、对不上实际发生的事(同
        // tournament 里 on_comparison_start 的顺序，理由一样)。
        if (on_cancel && on_cancel()) return cancelled_result();
        if (on_eval_progress) on_eval_progress(i + 1, eval_total);
        // 单张失败不中断整批：它只是没有描述可用，由票 06 的选择那一步处
        // 理，跟"某簇比较失败就那一簇退化、不中断其它簇"是同一个立场。
        (void)evaluate_fn(db, to_evaluate[i]);
      }
      // 评估跟锦标赛不一样，写库是逐张发生的，所以中途取消**不是零写
      // 入**：已经评估完的那几张会留在库里。这不是遗漏 - 每条记录本身都
      // 是完整的，留着正好被下一次运行的缓存判据命中，比回滚掉再花一次钱
      // 好。零写入的承诺只对闸门(ai_declined)成立，那时一张都还没评估。
      if (on_cancel && on_cancel()) return cancelled_result();

      // AI 开：从预选集里随机挑 count 个(PRD 已拍板接受不可复现，见
      // curate.h 的说明) - 没有质量分可比，"哪个 winner 更该被选中"本来
      // 就没有确定性答案。票 06 起改由模型读着描述连选带排。
      std::mt19937 rng(std::random_device{}());
      std::sample(winners.begin(), winners.end(), std::back_inserter(selected), count, rng);
    } else {
      // AI 关：farthest-point 多样性，逻辑不变，只是输入源从旧
      // build_cluster_reps 换成 winners(ai_enabled=false 时两者等价)，票
      // 04 之后 pool 可能已经被裁剪过。
      auto selected_info = take_farthest_points(pool, count);
      // 票 01：选中的是哪几张仍由 farthest-point 决定，但交出去的顺序不
      // 是它的挑选顺序——那个算法每次挑离已选集最远的一张，排列在时间上
      // 必然跳跃，而这个列表顺序一路决定 Deliver 的发送次序。
      std::sort(selected_info.begin(), selected_info.end(), by_captured_at_desc);
      for (const auto& r : selected_info) selected.push_back(r.id);
    }
  } else {
    // 簇数 < N：两种模式都返回全部 winner，不分 ai_enabled——没有"选"这
    // 个动作，谈不上随机还是多样性，统一按 captured_at 降序、id 升序排
    // 序(确定性)。不回填同簇的非 winner 成员，理由同旧版本：同簇成员是
    // "近似重复"或"同一场景"，回填会让最终结果出现彼此近重复的图，违背
    // curate 存在的多样性目的。
    std::vector<RepInfo> reps;
    for (auto id : winners) reps.push_back(make_rep_info(db, id));
    std::sort(reps.begin(), reps.end(), by_captured_at_desc);
    for (auto& r : reps) selected.push_back(r.id);
  }

  return CurateResult{selected, count, static_cast<int>(selected.size()),
                       summary.ai_fallback_count};
}

namespace {

// production 的评估一张图：解码预览图 -> request_evaluation -> 落库。跟
// EvaluationWorker::process_request_impl 是同一条链路，区别只在这里是同步
// 的、跑在调用线程上-curate 要在一次 headless 调用内部把预选集串行评估
// 完，用不了那套异步队列。
//
// 三个刻意的取值：
// - extra_guidance 传空串。用途**不**注入评估(PRD 决策五)：描述回答"这张
//   照片是什么"，是关于照片的客观事实；用途回答"这些事实里哪些重要"，是
//   关于这次任务的。注进去还会让缓存只对一种用途有效。
// - auto_reject 传 false。curate 是"挑哪几张"，不该顺手改变废片标签-那
//   会改变下一次运行的候选集，是一个用户没要求过的副作用。
// - language 用 request_evaluation 的默认值(中文)。curate 只有 headless
//   一个入口，core 不认识 cli 的界面语言；content 是给机器读的、不进
//   TUI，assessment 在这条路径上也不展示给人。票 07 的文案如果需要跟随
//   界面语言，那时再把语言接进来。
bool evaluate_and_store(db::Database& db, project::ImageId image_id, ai::Provider provider,
                        const ai::LocalModelConfig& local_config) {
  auto info = project::get_image(db, image_id);
  if (!info) return false;
  auto project_summary = project::open_project(db, info->project_id);
  if (!project_summary.ok()) return false;

  std::string path = media::resolve_preview_path(project_summary.value().root_path, info->file_path,
                                                  info->kind, info->preview_cache_path);
  auto decoded = media::decode_preview_file(path);
  if (!decoded.ok()) return false;

  auto evaluated = ai::request_evaluation(decoded.value(), /*extra_guidance=*/"", provider,
                                           ai::Language::Chinese, local_config);
  if (!evaluated.ok()) return false;
  return ai::store_evaluation(db, image_id, evaluated.value(), /*extra_guidance=*/"", provider,
                              /*auto_reject=*/false)
      .ok();
}

}  // namespace

CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold, double preselect_multiplier,
                     bool ai_enabled, ai::Provider ai_provider,
                     const ai::LocalModelConfig& local_config, dedup::DedupProgressFn on_progress,
                     dedup::AiProgressFn on_ai_progress, CurateAiGateFn on_ai_gate,
                     EvalProgressFn on_eval_progress, dedup::CancelFn on_cancel) {
  return detail::curate_impl(
      db, project_id, candidate_scope, count, time_window_seconds, hash_threshold,
      preselect_multiplier, ai_enabled, ai_provider, local_config,
      [ai_provider, &local_config](db::Database& d, project::ImageId id) {
        return evaluate_and_store(d, id, ai_provider, local_config);
      },
      std::move(on_progress), std::move(on_ai_progress), std::move(on_ai_gate),
      std::move(on_eval_progress), std::move(on_cancel));
}

}  // namespace pzt::core::curate
