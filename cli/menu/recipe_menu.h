#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cli/ui/ui.h"
#include "core/api.h"

// `r` 前缀键的风格(recipe)菜单交互。见 docs/history/M1_Eng_Design.md increment 6。
namespace pzt::cli::menu {

// `r` 键的入口。选中即应用/清除，不需要额外确认，参照标签系统的交互
// 哲学；应用/清除成功后返回空字符串(静默)，信息栏下一帧自然显示新的
// "配方:"状态——跟 handle_add_tag_result 成功时静默是同一个理由。创建/
// 删除是相对少见、更值得确认的操作，返回非空的状态提示。
enum class RKeyAction { Cancelled, Applied, Cleared, Toggled, Handled };
struct RKeyOutcome {
  RKeyAction action;
  std::string status;
};

// issue #20：`r c` 向导每前进一步就要拿"当前已知的完整参数组合"把正在浏览
// 的这张图重画一次。怎么画只有 browse.cpp 知道(它握着解码结果、降采样尺
// 寸和 kitty 的绘制参数),所以这里只收一个回调,不让 cli/menu 反向依赖
// cli/kitty 与 core/decode(issue #17 决策三)。
//
// 带预设 id 是因为预览要的是"这个预设的底子 + 这组草稿",而预设是在
// handle_r_create_flow 内部选的,调用方注入回调时还不知道是哪一个。
//
// 空的 std::function 表示"这次不需要预览"(当前图片没解码出来、或者调用方
// 根本不画图),向导照常走完,只是不显示画面。
using PreviewFn =
    std::function<void(pzt::core::RecipeId preset_id, const pzt::core::VersionParams& draft)>;

RKeyOutcome handle_r_key(pzt::core::ImageId image_id, int banner_row, int start_col,
                         int content_cols, const PreviewFn& preview = {});

// T-15 票 C（issue #33）：`/recipe <作用域>` 回车之后弹的配方菜单，决策
// D-2 的"菜单接力"。作用域写在控制台、配方在这里选 - 不做成
// `/recipe <作用域> <配方名>` 一行式，因为自定义配方的名字是
// `std::optional<std::string>`、可以为空且无唯一约束，一行式会把用户自建
// 的配方整个排除在批量之外。
//
// 这个菜单是 `r` 菜单的**子集**（决策 D-14）：只有 `1`-`9`（选预设 → 二
// 级选具体 version）与 `0`/`r`（批量清除），`v`/`c`/`d` 不出现。它不像
// handle_r_key 那样收 ImageId，因为批量语境下根本没有"这张图"-那正是
// `v` 与 `c` 在这里不成立的原因。
//
// 也不落库：选完就返回，写入由调用方在**确认之后**用
// core::set_images_recipe 一次做掉。菜单不写库是这一票的要害 - D-9 的确
// 认必须夹在选择和写入中间，把写入留在菜单里就没有那个位置了。
struct BatchRecipeSelection {
  // 取消（Esc、二级菜单取消、按了个不对应任何选项的键）。为真时
  // recipe_id 没有意义，调用方零写入。
  bool cancelled = true;
  // cancelled 为假时：有值 = 套这个配方，nullopt = 批量清除。
  std::optional<pzt::core::RecipeId> recipe_id;
  // 非空时是给用户的一句提示（"该预设不存在"这类），跟 RKeyOutcome::status
  // 同一个约定：Esc 静默、按错键给一句。
  std::string status;
};
BatchRecipeSelection handle_batch_recipe_menu(int banner_row, int start_col, int content_cols);

// issue #19：`r c` 分步向导的导航循环。按下标依次问 field_count 个字段，
// read_field(下标, 该字段当前值) 返回提交/回退/取消：提交前进一格，回退
// 退一格(在第一个字段上无效，停在原地)，取消整个作废并返回 nullopt。走
// 完最后一个字段返回全部字段的值,顺序与下标一致。
//
// 读一个字段这件事作为参数传进来而不是写死成 read_text_line_for_wizard,
// 是为了让导航本身能不带 tty 地测(见 cli/tests/recipe_menu_test.cpp):
// 这个循环里"回退到哪、回填什么值、第一个字段会不会越界"才是容易写错的
// 部分,而它一个终端字节都不需要碰。
//
// issue #20：on_values_changed 交出"用户此刻该看到的那一组值",拿到的是当
// 前全部字段(没填的是空串)。触发点有两个:
//   1. 问第一个字段之前一次,值全空 —— 用户填第一格时得有参照物,而且第一
//      格填 0(语义是"不调整")按 Enter 不该突然重渲染一次,那看起来像是输
//      入 0 产生了效果,实际变的是从旧风格切到了预设的底子(真机反馈);
//   2. 每次提交之后一次。
// 回退不在其中:回退只挪 index、不动任何值,所以回退之后屏幕上那一帧已经就
// 是"回到的那一步对应的组合",不需要也不值得再花一次渲染画一模一样的东
// 西。可以不传。
std::optional<std::vector<std::string>> run_field_wizard(
    std::size_t field_count,
    const std::function<pzt::cli::ui::WizardLineResult(std::size_t, const std::string&)>&
        read_field,
    const std::function<void(const std::vector<std::string>&)>& on_values_changed = {});

// issue #20：把向导的字段文本映射成一组完整的调整参数。前 8 格按向导顺序
// (高光/暗光/白平衡红/白平衡蓝/对比度/饱和度/黑色/白色)对应 VersionParams
// 的 8 个旋钮,第 9 格是名字、不参与;空串、解析不出数字、以及尾部有残渣的
// (如 "12abc")一律当 0——"静默归零"这条既有哲学不变(见 handle_r_create_flow)。
// values 短于 8 格时缺的那些也是 0,向导中途正是这个形状。
//
// 单独抽出来是因为预览与最终 create_version 必须走同一份映射:两边各写一
// 遍的话,顺序错一格就变成"照着预览调出来的参数,存下来不是那张图",而那
// 恰恰是实时预览这个功能的全部意义。
pzt::core::VersionParams params_from_wizard_fields(const std::vector<std::string>& values);

}  // namespace pzt::cli::menu
