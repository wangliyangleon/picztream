#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/api.h"

// `g` 前缀键的筛选/导出菜单。见 docs/M0_Eng_Design.md increment 6.4.6/6.6。
namespace pzt::cli::menu {

// increment 6.4.6:g + 数字切换到只浏览某个标签下图片的筛选视图,g + g
// 清除筛选。落地这个决定需要改 cmd_open 自己的 images/current_id/
// prefetch——这几个是 cmd_open 函数作用域内的局部变量,不像其它 handle_*
// 那样能靠传值参数完成整个动作,所以这里只返回"意图",不直接执行。
// increment 6.6:g + e 导出,跟前两者不同——它是个完全自包含的动作(选标
// 签 + 读路径 + 调 export_tag),不需要 cmd_open 再回头改 images/
// current_id,所以直接在这里(连同 handle_g_export_flow)把它执行完,用
// `Handled` 携带结果文案返回,风格上更接近 handle_space_key 那种"直接返回
// 状态提示"的做法,而不是 ApplyFilter/ClearFilter 那种"只返回意图"的做法。
enum class GKeyAction { Cancel, ClearFilter, ApplyFilter, Handled };

struct GKeyDecision {
  GKeyAction action = GKeyAction::Cancel;
  pzt::core::TagId tag_id{};  // 只有 action == ApplyFilter 时有意义
  std::string tag_name;       // 同上,顺便带出来给信息栏筛选提示用,不用每
                              // 帧再查一次 tags_for_menu 只为了显示名字
  std::string status;         // 只有 action == Handled 时有意义
};

// g 键入口:显示筛选/导出/清除选项,返回一个"意图"(GKeyDecision)交给
// cmd_open 执行(g + e 导出是自包含的,直接在内部执行完用 Handled 返回)。
// tags 由调用方(cmd_open)用 tags_for_menu 构好后传入。
GKeyDecision handle_g_key_prompt(pzt::core::TagId reject_tag_id,
                                 const std::vector<pzt::core::TagSummary>& tags,
                                 std::optional<pzt::core::TagId> active_filter_tag_id,
                                 const std::string& active_filter_tag_name, int banner_row,
                                 int start_col, int content_cols);

}  // namespace pzt::cli::menu
