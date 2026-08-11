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
// "风格:"状态——跟 handle_add_tag_result 成功时静默是同一个理由。创建/
// 删除是相对少见、更值得确认的操作，返回非空的状态提示。
enum class RKeyAction { Cancelled, Applied, Cleared, Toggled, Handled };
struct RKeyOutcome {
  RKeyAction action;
  std::string status;
};

RKeyOutcome handle_r_key(pzt::core::ImageId image_id, int banner_row, int start_col,
                         int content_cols);

// issue #19：`r c` 分步向导的导航循环。按下标依次问 field_count 个字段，
// read_field(下标, 该字段当前值) 返回提交/回退/取消：提交前进一格，回退
// 退一格(在第一个字段上无效，停在原地)，取消整个作废并返回 nullopt。走
// 完最后一个字段返回全部字段的值,顺序与下标一致。
//
// 读一个字段这件事作为参数传进来而不是写死成 read_text_line_for_wizard,
// 是为了让导航本身能不带 tty 地测(见 cli/tests/recipe_menu_test.cpp):
// 这个循环里"回退到哪、回填什么值、第一个字段会不会越界"才是容易写错的
// 部分,而它一个终端字节都不需要碰。
std::optional<std::vector<std::string>> run_field_wizard(
    std::size_t field_count,
    const std::function<pzt::cli::ui::WizardLineResult(std::size_t, const std::string&)>&
        read_field);

}  // namespace pzt::cli::menu
