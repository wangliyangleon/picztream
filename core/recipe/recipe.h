#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/db/database.h"

// Recipe（色彩配方）模块。见 docs/M1_Eng_Design.md "core/recipe/" 一节。两
// 层模型（内置预设 / 用户在预设基础上保存的 version）用同一张自引用的
// `recipes` 表表达，这个模块提供的类型和函数对两层一视同仁，区分靠
// `parent_id` 是否为空。这次(increment 1)只落地只读查询和预设的种子数
// 据，version 的创建/改名/软删除是 increment 2 的事。
namespace pzt::core::recipe {

using RecipeId = std::int64_t;

struct PresetSummary {
  RecipeId id;
  std::string name;
};

struct VersionSummary {
  RecipeId id;
  RecipeId preset_id;
  std::optional<std::string> name;
  double highlights;
  double shadows;
  double wb_shift_r;
  double wb_shift_b;
  bool deleted;
};

// 内置预设，parent_id IS NULL 的那些行，按 id 升序（创建顺序）。
std::vector<PresetSummary> list_presets(db::Database& db);

// 某个预设下用户保存的 version，含软删除的（deleted=true），调用方按场景
// 过滤——`r` 菜单只展示未删除的，`pzt recipe list` 全部展示并标注状态。
// increment 1 还没有创建 version 的入口，这个函数永远返回空列表。
std::vector<VersionSummary> list_versions(db::Database& db, RecipeId preset_id);

// 数据库第一次初始化、`recipes` 表刚建出来还没有任何内置预设时播种——用
// INSERT OR IGNORE 配合 schema 里预设名字的局部唯一索引保证幂等，调用多
// 次只有第一次真正插入。占位内容（"Standard"=恒等 LUT，"Warm"=一个随手
// 调的暖色偏移）只用来验证整条机制通不通，真正的调色设计留到后面随时可
// 以补充，不阻塞其它 increment。
void ensure_default_presets(db::Database& db);

}  // namespace pzt::core::recipe
