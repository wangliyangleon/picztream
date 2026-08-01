#pragma once

#include <optional>
#include <vector>

#include "core/ai/ai.h"
#include "core/db/database.h"
#include "core/dedup/dedup.h"  // DedupProgressFn / AiProgressFn(两者都定义在这里)
#include "core/project/project.h"
#include "core/tagging/tagging.h"

// 策展挑图算法。见 docs/history/M4_Eng_Design.md 第三节"Curate 算法设计"——从非
// 废片/非重复的候选图里挑 N 张兼顾多样性的代表作(W2026-07-21：候选不再
// 依赖是否评估过，见 core/tournament 的说明)。这里只负责选择，不打标
// 签、不导出（单一职责）；`pzt curate` 命令拿到结果后自己去调 add_tag
// 落用户指定的标签。
namespace pzt::core::curate {

struct CurateResult {
  // 有序，且这个顺序有意义：调用方(pzt curate -> agent -> Deliver)按它的
  // 次序发送，决定九宫格位置与轮播先后。票 01 起两条确定性路径(关 AI 凑够
  // count、候选簇数不足 count)都是 captured_at 降序、id 升序；开 AI 且候选
  // 够那条仍是 std::sample 的保序输出(即簇遍历顺序)，票 06 改由模型决定。
  std::vector<project::ImageId> selected;
  int requested;
  int returned;  // == selected.size()，< requested 表示候选不足
  // W2026-07-21 目标二：ai_enabled=true 时，因为某次 AI 比较失败而整簇
  // 退化成"选 captured_at 最新"的簇数；ai_enabled=false 时恒为 0。见
  // core::tournament::ChooseSummary 同名字段。
  int ai_fallback_count;
};

// candidate_scope 为空 => 候选范围是整个项目;否则限定到某标签下。
// count > 0。time_window_seconds/hash_threshold 是分簇复用的
// dedup::find_duplicates 参数——调用方(pzt curate 命令)从
// Settings.curate_time_window_seconds/curate_hash_threshold 显式传入
// (curate 本身不读 Settings，跟 dedup::find_duplicates 同一约定)。
// project_id 由调用方保证已存在(headless 命令调用前已经过
// resolve_project_json 校验)。
//
// ai_enabled/ai_provider/local_config(W2026-07-21 目标二新增)：默认
// ai_enabled=false，保证现有调用点零改动。ai_enabled=false 时每簇选
// captured_at 最新的代表，凑够 count 张走现有 farthest-point 多样性；
// ai_enabled=true 时每簇改走单淘汰锦标赛选出 winner，凑够 count 张时从
// winner 集合里随机挑(不做种子化，接受不可复现)。两种模式下候选簇数不
// 足 count 时都返回全部 winner，见 core::tournament::cluster_and_choose
// 的说明。
// on_progress/on_ai_progress（T-8）：原样转给 cluster_and_choose，语义见
// core::dedup::DedupProgressFn 与 core::tournament::AiProgressFn。默认
// nullptr，现有调用点零改动，跟 W2026-07-21 给 dedup 加参数时同一个约
// 定。on_ai_progress 无条件传即可——core 只在 ai_enabled 时才会调它。
//
// 刻意不补的还有四项（on_ai_gate、on_cancel、返回结构的 ai_declined/
// cancelled），它们在 dedup 那边有、这边没有。判据是有没有消费者，不是
// 对不对称：`/curate` 按 SPEC §3.2 不进 TUI（没人问闸门），agent 的取消
// 是进程级 terminate。**将来谁给 curate 加上 on_ai_gate 或 on_cancel，
// 必须在同一个改动里给 CurateResult 补上 ai_declined/cancelled**——
// tournament::ChooseSummary 早就产出了这两个字段，而 curate.cpp 里那句
// `if (summary.clusters.empty())` 会把它们连同"什么都没做"的语义一起丢
// 掉，跟"候选池本来就是空的"折叠成同一个返回值。今天不可达只是因为这
// 两个钩子都没传。见 docs/history/Headless_Observability_PRD.md 决策七及其附注。
// preselect_multiplier（票 04 的 M）：选片之前先把候选集(每簇一张代表)
// 按时间多样性裁成预选集，规模 = min(ceil(max(1.5, M) · count), 候选集
// 大小)，选择逻辑本身不变、只是面对一个更小的池子。跟
// time_window_seconds/hash_threshold 同一个约定——curate 不读 Settings，
// 由调用方从 Settings.curate_preselect_multiplier 显式传入。默认 2 与
// Settings 的默认值一致。见 docs/Intent_Curation_PRD.md 决策十至十二：
// 这一刀跑在(将来的)评估之前，因此多样性只沿 captured_at 衡量，也因此评
// 估次数由构造保证有界、与图库大小无关。候选集不足 count 时整条裁剪不参
// 与(那时没有"选"这个动作)。
CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold,
                     double preselect_multiplier = 2.0,
                     bool ai_enabled = false, ai::Provider ai_provider = ai::Provider::Local,
                     const ai::LocalModelConfig& local_config = ai::LocalModelConfig{},
                     dedup::DedupProgressFn on_progress = nullptr,
                     dedup::AiProgressFn on_ai_progress = nullptr);

namespace detail {

// 预选集大小 = min(ceil(max(1.5, multiplier) · count), candidate_count)。
// 暴露出来只为可测：这是票 04 里最需要穷举边界的一块(下界 1.5、上界候选
// 集大小、ceil 的取整方向)，摘成不碰数据库和网络的纯函数才能表驱动覆盖，
// 跟 core/ai 里 detail::request_*_impl 是同一个先例。candidate_count 或
// count 非正时返回 0(curate 的契约保证 count > 0，这里只保证不返回负数)。
int preselect_size(int candidate_count, double multiplier, int count);

}  // namespace detail

}  // namespace pzt::core::curate
