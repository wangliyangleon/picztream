#pragma once

#include <optional>
#include <vector>

#include "core/ai/ai.h"
#include "core/db/database.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

// 策展挑图算法。见 docs/M4_Eng_Design.md 第三节"Curate 算法设计"——从非
// 废片/非重复的候选图里挑 N 张兼顾多样性的代表作(W2026-07-21：候选不再
// 依赖是否评估过，见 core/tournament 的说明)。这里只负责选择，不打标
// 签、不导出（单一职责）；`pzt curate` 命令拿到结果后自己去调 add_tag
// 落用户指定的标签。
namespace pzt::core::curate {

struct CurateResult {
  std::vector<project::ImageId> selected;  // 有序:入选顺序
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
CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold,
                     bool ai_enabled = false, ai::Provider ai_provider = ai::Provider::Local,
                     const ai::LocalModelConfig& local_config = ai::LocalModelConfig{});

}  // namespace pzt::core::curate
