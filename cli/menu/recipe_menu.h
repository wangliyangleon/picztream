#pragma once

#include <string>

#include "core/api.h"

// `r` 前缀键的风格(recipe)菜单交互。见 docs/M1_Eng_Design.md increment 6。
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

}  // namespace pzt::cli::menu
