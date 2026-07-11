#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/api.h"

// `space` 键的标签菜单交互(加/摘/新建/删除标签)。见 docs/M0_Eng_Design.md
// increment 6.4.4。
namespace pzt::cli::menu {

// space 菜单的数字键 1-8 对应的标签列表:过滤掉系统标签(废片固定占 0,
// 重复固定占 9,见 F-01)、按 tag id 升序固定编号、截断到 8 个。space
// 菜单和 `pzt open` 主循环里的 `g` 筛选菜单共用这一套编号。
std::vector<pzt::core::TagSummary> tags_for_menu(pzt::core::ProjectId project_id);

// 处理 add_tag 结果:成功静默;cap 超限转入替换子菜单。数字加标签分支和
// `x` 快捷键共用这段逻辑,所以是 public(cmd_open 的 `x` 直接调它)。
std::string handle_add_tag_result(pzt::core::TagId tag_id, pzt::core::ImageId image_id,
                                  int banner_row, int start_col, int content_cols);

// space 键的入口:显示可选标签、读一个键选标签或转入 -/c/d 子流程。
// F-01：duplicate_tag_id 为空表示项目还没有"重复"系统标签(没跑过
// /dedup),这种情况下 `9` 不出现在菜单里、按了也不响应；调用方
// (cmd_open)每次按 space 前都重新查一次,不缓存(dedup 可能在同一次
// 浏览会话里第一次创建这个标签)。
std::string handle_space_key(pzt::core::ProjectId project_id, pzt::core::TagId reject_tag_id,
                             std::optional<pzt::core::TagId> duplicate_tag_id,
                             pzt::core::ImageId image_id, int banner_row, int start_col,
                             int content_cols);

}  // namespace pzt::cli::menu
