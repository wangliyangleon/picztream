#pragma once

#include <string>
#include <vector>

#include "core/api.h"

// `space` 键的标签菜单交互(加/摘/新建/删除标签)。见 docs/M0_Eng_Design.md
// increment 6.4.4。
namespace pzt::cli::menu {

// space 菜单的数字键 1-9 对应的标签列表:过滤掉系统标签(废片固定占 0)、
// 按 tag id 升序固定编号、截断到 9 个。space 菜单和 `pzt open` 主循环里
// 的 `g` 筛选菜单共用这一套编号。
std::vector<pzt::core::TagSummary> tags_for_menu(pzt::core::ProjectId project_id);

// 处理 add_tag 结果:成功静默;cap 超限转入替换子菜单。数字加标签分支和
// `x` 快捷键共用这段逻辑,所以是 public(cmd_open 的 `x` 直接调它)。
std::string handle_add_tag_result(pzt::core::TagId tag_id, pzt::core::ImageId image_id,
                                  int banner_row, int start_col, int content_cols);

// space 键的入口:显示可选标签、读一个键选标签或转入 -/c/d 子流程。
std::string handle_space_key(pzt::core::ProjectId project_id, pzt::core::TagId reject_tag_id,
                             pzt::core::ImageId image_id, int banner_row, int start_col,
                             int content_cols);

}  // namespace pzt::cli::menu
