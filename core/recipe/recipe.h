#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/result.h"

// Recipe（色彩配方）模块。见 docs/M1_Eng_Design.md "core/recipe/" 一节。两
// 层模型（内置预设 / 用户在预设基础上保存的 version）用同一张自引用的
// `recipes` 表表达，这个模块提供的类型和函数对两层一视同仁，区分靠
// `parent_id` 是否为空。
namespace pzt::core::recipe {

using RecipeId = std::int64_t;
using project::ImageId;

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
std::vector<VersionSummary> list_versions(db::Database& db, RecipeId preset_id);

// 数据库第一次初始化、`recipes` 表刚建出来还没有任何内置预设时播种——用
// INSERT OR IGNORE 配合 schema 里预设名字的局部唯一索引保证幂等，调用多
// 次只有第一次真正插入。占位内容（"Standard"=恒等 LUT，"Warm"=一个随手
// 调的暖色偏移）只用来验证整条机制通不通，真正的调色设计留到后面随时可
// 以补充，不阻塞其它 increment。
void ensure_default_presets(db::Database& db);

// increment 2:version 的增删改。这四个调整参数是这次先落地的最小集合
// (高光/暗光/白平衡红蓝偏移)，以后想加色温/锐度/对比度之类，走跟
// `images.recipe_id` 一样的 ensure_column 迁移机制加新列，不需要现在改
// 成更"灵活"但类型不安全的 JSON blob 之类的设计。
struct VersionParams {
  double highlights = 0;
  double shadows = 0;
  double wb_shift_r = 0;
  double wb_shift_b = 0;
};

enum class CreateVersionError {
  PresetNotFound,  // 包括"这个 id 存在但本身是个 version,不是预设"的情况
};

Result<RecipeId, CreateVersionError> create_version(db::Database& db, RecipeId preset_id,
                                                     std::optional<std::string> name,
                                                     VersionParams params);

enum class RecipeOpError {
  NotFound,  // 同时覆盖"id 不存在"和"已经软删除过"——跟 delete_tag 对不存
             // 在的 tag_id 报错是同一种"实体级操作不是幂等的"精神，再删一
             // 次已经软删除的 version 不当成静默成功
  IsPreset,  // 预设不可改名/不可删除
};

Result<void, RecipeOpError> rename_version(db::Database& db, RecipeId version_id,
                                            const std::string& new_name);

// 软删除:设置 deleted_at，不影响已经引用这个 version 的图片渲染，只是从
// "应用/创建"的可选列表里隐藏。见 docs/M1_PRD.md 里软删除的完整语义说明。
Result<void, RecipeOpError> delete_version(db::Database& db, RecipeId version_id);

// increment 3:图片 ↔ recipe 关联。
enum class SetImageRecipeError {
  ImageNotFound,
  RecipeNotFound,  // 包括"不存在"和"是个已经软删除的 version"两种情况
};

// recipe_id = nullopt 就是清除(Origin)。recipe_id 可以指向一个预设本身
// (应用它的中性状态)或者某个 version——这里的校验对两者一视同仁,只要求
// 这一行存在且没有被软删除,不区分是预设还是 version。
Result<void, SetImageRecipeError> set_image_recipe(db::Database& db, ImageId image_id,
                                                    std::optional<RecipeId> recipe_id);

// 图片不存在、或者存在但没应用任何 recipe，两种情况都返回空——跟
// tags_for_image 对不存在的 image_id 返回空列表是同一个套路，不特殊区分。
std::optional<RecipeId> get_image_recipe(db::Database& db, ImageId image_id);

// 把一个 recipe_id 解析成"预设名 + 可选的 version 名"这对结构化数据，
// 不在 core 里拼成一整行文本——展示成一行还是缩进的两层树状结构是 cli
// 的排版决定，不是 core 的事。version_name 为空表示直接应用的是预设本
// 身(没有第二层)；version 没设名字时这里用 "(未命名)" 占位字符串填进
// version_name，调用方不需要再判一次内层是不是空。不存在的 id 返回空。
struct RecipeDescription {
  std::string preset_name;
  std::optional<std::string> version_name;
};
std::optional<RecipeDescription> describe_recipe(db::Database& db, RecipeId recipe_id);

}  // namespace pzt::core::recipe
