#include "cli/menu/filter_menu.h"

#include <optional>
#include <string>
#include <vector>

#include "cli/text/text.h"
#include "cli/ui/ui.h"
#include "cli/i18n/i18n.h"

// prompt_and_read_key/read_text_line 来自 cli/ui,expand_home_path 来自
// cli/text,用 using-directive 让搬过来的函数体保持逐字不变。
using namespace pzt::cli::text;
using namespace pzt::cli::ui;

namespace pzt::cli::menu {
namespace {

// g + e 之后:选导出目标标签(数字编号同 g 菜单,或者当前处于筛选视图时按
// `e` 表示"就导出这个筛选标签",省一次选择)、读目标路径、调用
// export_tag。固定用 LinkMode::Copy——软链场景用独立的 `pzt export
// --link`,这个快捷方式不做模式切换,见 docs/M0_Eng_Design.md increment
// 6.6 的说明。空路径不是 Esc,得给个反馈而不是静默(跟 handle_create_tag_
// flow 空标签名的处理一致);Esc 在任一步都中止整个流程,静默。
std::string handle_g_export_flow(pzt::core::TagId reject_tag_id,
                                  const std::vector<pzt::core::TagSummary>& tags,
                                  std::optional<pzt::core::TagId> active_filter_tag_id,
                                  const std::string& active_filter_tag_name, int banner_row,
                                  int start_col, int content_cols) {
  std::string line = pzt::cli::i18n::filter_menu_export_prefix();
  if (active_filter_tag_id) line += pzt::cli::i18n::filter_menu_export_current(active_filter_tag_name);
  line += "0:" + pzt::cli::i18n::reject_tag_label();
  for (std::size_t i = 0; i < tags.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + tags[i].name;
  }
  line += pzt::cli::i18n::tag_menu_esc_cancel();
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  pzt::core::TagId target_id;
  std::string target_name;
  if (c == 'e' && active_filter_tag_id) {
    target_id = *active_filter_tag_id;
    target_name = active_filter_tag_name;
  } else if (c == '0') {
    target_id = reject_tag_id;
    target_name = pzt::cli::i18n::reject_tag_label();
  } else if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
    const auto& t = tags[static_cast<std::size_t>(c - '1')];
    target_id = t.id;
    target_name = t.name;
  } else {
    return "";  // 取消,静默(含 `e` 但没有筛选生效的情形)
  }

  auto path = read_text_line(pzt::cli::i18n::filter_menu_export_to_prompt(), banner_row, start_col, content_cols);
  if (!path) return "";  // Esc,静默取消
  if (path->empty()) return pzt::cli::i18n::filter_menu_export_path_empty();
  std::string resolved_path = expand_home_path(*path);

  auto result = pzt::core::export_tag(target_id, resolved_path, pzt::core::LinkMode::Copy);
  if (!result.ok()) {
    if (result.error() == pzt::core::ExportTagError::IoError) {
      return pzt::cli::i18n::filter_menu_export_io_error(resolved_path);
    }
    return pzt::cli::i18n::filter_menu_export_failed();
  }

  const auto& r = result.value();
  if (r.exported_count == 0 && r.skipped.empty()) {
    return pzt::cli::i18n::filter_menu_export_no_images(target_name);
  }
  return pzt::cli::i18n::filter_menu_export_success(r.exported_count, target_name, resolved_path, r.created_output_folder, r.skipped.size());
}

}  // namespace

GKeyDecision handle_g_key_prompt(pzt::core::TagId reject_tag_id,
                                  const std::vector<pzt::core::TagSummary>& tags,
                                  std::optional<pzt::core::TagId> active_filter_tag_id,
                                  const std::string& active_filter_tag_name, int banner_row,
                                  int start_col, int content_cols) {
  std::string line = pzt::cli::i18n::filter_menu_main_prompt(tags);
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c == 'g') return {GKeyAction::ClearFilter, {}, "", ""};
  if (c == 'e') {
    std::string status = handle_g_export_flow(reject_tag_id, tags, active_filter_tag_id,
                                               active_filter_tag_name, banner_row, start_col,
                                               content_cols);
    return {GKeyAction::Handled, {}, "", status};
  }
  if (c == '0') {
    return {GKeyAction::ApplyFilter, reject_tag_id, pzt::cli::i18n::reject_tag_label(), ""};
  }
  if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
    const auto& t = tags[static_cast<std::size_t>(c - '1')];
    return {GKeyAction::ApplyFilter, t.id, t.name, ""};
  }
  return {GKeyAction::Cancel, {}, "", ""};  // 取消,静默,跟其它菜单一致
}

}  // namespace pzt::cli::menu
