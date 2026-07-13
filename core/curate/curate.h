#pragma once

#include <optional>
#include <vector>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

// 策展挑图算法。见 docs/M4_Eng_Design.md 第三节"Curate 算法设计"——从已
// 评估、未废片/未重复的候选图里挑 N 张兼顾质量与多样性的代表作。这里只
// 负责选择，不打标签、不导出（单一职责）；`pzt curate` 命令拿到结果后
// 自己去调 add_tag 落用户指定的标签。
namespace pzt::core::curate {

struct CurateResult {
  std::vector<project::ImageId> selected;  // 有序:入选顺序
  int requested;
  int returned;  // == selected.size()，< requested 表示候选不足
};

// candidate_scope 为空 => 候选范围是整个项目;否则限定到某标签下。
// count > 0。time_window_seconds/hash_threshold 是分簇复用的
// dedup::find_duplicates 参数——调用方(pzt curate 命令)从
// Settings.curate_time_window_seconds/curate_hash_threshold 显式传入
// (curate 本身不读 Settings，跟 dedup::find_duplicates 同一约定)。
// project_id 由调用方保证已存在(headless 命令调用前已经过
// resolve_project_json 校验)。
CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold);

}  // namespace pzt::core::curate
