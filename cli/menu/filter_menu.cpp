#include "cli/menu/filter_menu.h"

#include <optional>
#include <string>
#include <vector>

#include "cli/ui/ui.h"
#include "cli/i18n/i18n.h"

// prompt_and_read_key 来自 cli/ui,用 using-directive 让搬过来的函数体
// 保持逐字不变。
using namespace pzt::cli::ui;

namespace pzt::cli::menu {

// 点 1：g+e"挑任意标签导出"曾经的入口(交互起来非常诡异)已经退休——导
// 出统一走顶层 `e` 键(见 cli/commands/browse.cpp 的说明)，`g` 菜单只
// 负责筛选，不再兼管导出，不再需要知道 active filter 是哪个标签。
GKeyDecision handle_g_key_prompt(pzt::core::TagId reject_tag_id,
                                  std::optional<pzt::core::TagId> duplicate_tag_id,
                                  const std::vector<pzt::core::TagSummary>& tags, int banner_row,
                                  int start_col, int content_cols) {
  // 标签一多,单行拼不下,拆成两行:第一行编号选项,第二行固定字母操
  // 作,见 prompt_and_read_key_2line 的说明。
  char c = prompt_and_read_key_2line(
      pzt::cli::i18n::filter_menu_options_line(tags, duplicate_tag_id.has_value()),
      pzt::cli::i18n::filter_menu_actions_line(), banner_row, start_col, content_cols);
  if (c == 'g') return {GKeyAction::ClearFilter, {}, "", ""};
  if (c == '0') {
    return {GKeyAction::ApplyFilter, reject_tag_id, pzt::cli::i18n::reject_tag_label(), ""};
  }
  // F-01：`9:重复` 跟 `0:废片` 对称，只在 duplicate_tag_id 有值时才响应。
  if (c == '9' && duplicate_tag_id) {
    return {GKeyAction::ApplyFilter, *duplicate_tag_id, pzt::cli::i18n::duplicate_tag_label(), ""};
  }
  if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
    const auto& t = tags[static_cast<std::size_t>(c - '1')];
    return {GKeyAction::ApplyFilter, t.id, t.name, ""};
  }
  if (c == 0x1B) return {GKeyAction::Cancel, {}, "", ""};  // Esc,静默
  // 不是 Esc,也不对应任何选项——跟 handle_r_key 一致,给一句反馈而不是
  // 完全没反应(真机反馈:直接退回一级菜单,分不清是没按对还是没反应)。
  return {GKeyAction::Cancel, {}, "", pzt::cli::i18n::recipe_menu_invalid_key()};
}

}  // namespace pzt::cli::menu
