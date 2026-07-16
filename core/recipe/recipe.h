#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/color/color.h"
#include "core/db/database.h"
#include "core/decode/decode.h"
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
// 次只有第一次真正插入。占位内容("Origin"=没有 base_lut，只用来承载亮
// 度/白平衡这类细节调整，不代表任何"风格"；"Warm"=一个随手调的暖色偏移
// LUT)只用来验证整条机制通不通，真正的调色设计留到后面随时可以补充，不
// 阻塞其它 increment。
//
// "Origin" 固定用 id=0 播种(照抄"废片"系统标签固定占 0 号位的先例)，其
// 它预设(目前只有 Warm)照常让 SQLite 自动分配 id。**待确认**:未来
// increment 6 落地真正的 `r` 菜单编号时，需要把 Origin 从"按创建顺序动
// 态编号 1-9"的列表里过滤掉、固定映射到数字 0(类比 `tags_for_menu` 过滤
// is_system 标签的做法)——目前所有预设都是 `is_system=1`，不能单靠这个
// 字段区分"Origin"和其它预设，需要按固定 id=0 或者名字判断。另外，`r`+
// `0`/`r`+`r`(快捷清除)继续走 `recipe_id = NULL` 这条路径,不改成指向
// Origin 预设本身——两者对"没有风格"这件事产出相同的视觉效果,但前者是
// 零查询的最快路径,后者(选中 Origin 预设)是留给"只想调亮度/白平衡、不
// 要任何风格化观感"这个场景的,两者并存，increment 6 设计交互时需要想清
// 楚怎么把这个区别对用户讲清楚,不要让两个"看起来都叫 Origin"的东西显得
// 混乱。
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

// increment 4:把一个 recipe_id 解析成"要用哪个预设的 LUT + 最终生效的
// 调整参数"。指向预设本身时 lut/size 来自这一行自己，params 全零(预设
// 自己就是中性状态)；指向 version 时 lut/size 来自 parent 预设，params
// 来自这一行。lut 里的数据是拷贝，不是指向 DB 查询结果的指针——那样的
// 指针在 sqlite3_stmt 析构之后就悬空了。
//
// lut 为空(std::nullopt)是合法状态，不是错误——固定的 "Origin" 预设(见
// ensure_default_presets)本身就没有 base_lut，代表"没有基础调色风格，只
// 有亮度/白平衡这类细节可调"，`render` 遇到这种情况直接跳过 apply_lut
// 那一步，不做一次没有意义的恒等 LUT 三线性插值——这不只是省事，是真的
// 省掉一次逐像素 8 次查表插值的计算量。对称地，`render` 在 params 四个
// 调整参数全零时也跳过 apply_adjustments(纯 Origin、或者任何调整全归零
// 的 version 都是这种情况)，理由一样：全零参数算出来的 delta 恒为
// 0、增益恒为 1，是个不折不扣的无意义计算。
//
// 对软删除的 version 一视同仁，只有 id 真不存在才返回空——这是跟
// set_image_recipe 刻意不同的地方:set_image_recipe 拒绝软删除的目标是
// 为了不让用户*新*选中一个已经删除的东西，但软删除的整个意义就是"已经
// 引用它的图片继续正常渲染"，resolve_recipe/render 必须对已经软删除的
// version 也能正常工作。
struct ResolvedRecipe {
  std::optional<color::Lut3D> lut;
  VersionParams params;
};
std::optional<ResolvedRecipe> resolve_recipe(db::Database& db, RecipeId recipe_id);

enum class RenderRecipeError {
  RecipeNotFound,
};

// 组合 resolve_recipe + core/color 的像素运算，是 core/recipe 对外唯一
// 需要碰 core/color 的地方。thread_count=1 用于预览(同步)，导出烘焙传
// hardware_concurrency()。
Result<decode::DecodedImage, RenderRecipeError> render(db::Database& db,
                                                        const decode::DecodedImage& src,
                                                        RecipeId recipe_id,
                                                        unsigned thread_count = 1);

// 仅供单元测试直接验证每个旋钮的效果——真正的调用方是 recipe.cpp 内部
// ensure_default_presets 给 9 个内置预设烘焙 LUT,不是面向 core/api 的公开
// 接口。照抄 core::ai::detail::downscale_for_upload 的先例(core/ai/ai.h)。
namespace detail {

// 5 个 -100..100 的百分比旋钮,烘焙成一份静态 LUT——不是 apply_adjustments
// 那种运行时按每个 version 调的实时参数,这里生成的 LUT 存进
// recipes.base_lut,跟预设本身一样是"烘焙好之后不再变"的。
struct GradeParams {
  double wb_shift_r = 0;
  double wb_shift_b = 0;
  double saturation = 0;
  double contrast = 0;
  double brightness = 0;
};

// 对每个网格点依次做 白平衡 -> 整体明暗 -> 对比度 -> 饱和度 四步,公式见
// docs/W2026-07-15_RecipeExpansion_Eng_Design.md。saturation=-100 时四步
// 结束后三通道必然相等(collapse 到 luma),黑白预设直接复用这同一个函数,
// 不需要单独的黑白代码路径。
std::vector<float> make_graded_lut(int n, const GradeParams& params);

}  // namespace detail

}  // namespace pzt::core::recipe
