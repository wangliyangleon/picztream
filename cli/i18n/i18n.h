#pragma once

#include <string>
#include <vector>
#include <optional>
#include "core/api.h"

namespace pzt::cli::i18n {

enum class Lang { zh, en };

extern Lang g_lang;

void init_lang();

// "废片"/reject 系统标签的显示名——单独抽出来是因为它跟其它 UI 文本不一
// 样,不是纯 cli 生成的字符串:这个标签的真实名字是 core::tagging::
// kRejectTagName,创建时就以固定的中文字面量写进数据库,`TagSummary::name`
// 读出来的永远是那个字面量,不会跟着 g_lang 变。任何直接显示 tag.name 的
// 地方,如果这个 tag 恰好是系统标签,都必须换成这个函数的返回值,否则英文
// 模式下会漏出裸的中文标签名。目前 is_system 只用在这一个标签上,所以直接
// 按 is_system 判断,不需要比较 tag_id。
std::string reject_tag_label();
std::string tag_display_name(const pzt::core::TagSummary& tag);

// 所有菜单里"按键 -> 可选项"统一格式:"key:[label]"。数量/状态这类附加
// 后缀(比如 tag 的 cap 计数、version 名字后面的"张")不属于 label 本身,
// 由调用方自己拼在括号外面,不传进这个函数。
std::string menu_item(const std::string& key, const std::string& label);

// Main Usage / Commands Help
std::string usage_main();
std::string usage_tag();
std::string usage_recipe();

// Command errors/status
std::string err_unknown_subcommand(const std::string& subcommand);
std::string err_project_not_found(const std::string& cmd, const std::string& project_name);
std::string err_new_missing_name();
std::string err_new_name_exists(const std::string& name);
std::string err_new_no_images(const std::string& folder_path);
std::string msg_project_created(const std::string& name, const std::string& root_path, long long image_count);
std::string msg_project_created_simple(const std::string& name);
std::string err_archive_missing_name();
std::string err_archive_failed(const std::string& name);
std::string msg_project_archived(const std::string& name);
std::string err_delete_missing_name();
std::string msg_delete_warn_prompt(const std::string& name);
std::string msg_delete_confirm_input();
std::string msg_delete_cancelled();
std::string err_delete_failed(const std::string& name);
std::string msg_project_deleted(const std::string& name);
std::string err_tag_list_missing_name();
std::string msg_tag_list_empty();
std::string msg_tag_item(const std::string& name, long long count, std::optional<std::int64_t> cap, bool is_ordered, bool is_system);
std::string msg_project_list_empty();
std::string msg_project_item(const std::string& name, long long image_count, const std::string& root_path, bool archived);
std::string err_rescan_missing_name();
std::string err_rescan_unknown_arg(const std::string& arg);
std::string err_rescan_failed(const std::string& name);
std::string msg_rescan_result(long long added, long long removed, long long total);
std::string err_export_missing_args();
std::string err_export_tag_not_found(const std::string& tag_name);
std::string err_export_io_error(const std::string& path);
std::string msg_export_no_images(const std::string& tag_name);
std::string msg_export_success(int count, const std::string& path, bool created_folder);
std::string msg_export_skipped(size_t count);
std::string msg_export_skipped_item(const std::string& file_name, const std::string& reason);
std::string export_skip_reason(pzt::core::SkipReason reason);
std::string err_tag_unknown_subcommand(const std::string& verb);
std::string err_recipe_list_no_args();
std::string msg_recipe_list_empty();
std::string msg_recipe_preset_item(int index, const std::string& name);
std::string msg_recipe_version_deleted_label();
std::string msg_recipe_version_unnamed_label();
std::string msg_recipe_version_item(int v, const std::string& name, double hi, double sh, double r, double b);
std::string err_recipe_rename_missing_args();
std::string err_recipe_rename_invalid_address(const std::string& addr);
std::string err_recipe_rename_not_found(const std::string& addr);
std::string err_recipe_rename_failed();
std::string msg_recipe_renamed(const std::string& new_name);
std::string err_recipe_delete_missing_args();
std::string err_recipe_delete_invalid_address(const std::string& addr);
std::string err_recipe_delete_not_found(const std::string& addr);
std::string err_recipe_delete_failed();
std::string msg_recipe_deleted(const std::string& addr);
std::string err_recipe_unknown_subcommand(const std::string& verb);

// Open / Browse UI
std::string err_open_project_not_found();
std::string err_open_project_no_images(const std::string& name);
std::string err_open_tmux_passthrough();
std::string banner_text();
std::string info_filter_label(const std::string& tag_name);
std::string info_tags_label();
std::string info_none_label();
std::string info_size_label(const std::string& size_str);
std::string info_style_label();
std::string info_style_none_label();
std::string msg_press_any_key_to_continue(const std::string& status);
std::string err_open_render_failed();
std::string err_open_decode_failed();
std::string msg_all_tagged();
std::string err_remove_tag_failed();
std::string err_filter_failed();
std::string msg_filter_no_images();
std::string msg_browse_exited();

// Tag Menu
std::string tag_menu_cap_zero();
std::string tag_menu_full(int cap);
std::string tag_menu_esc_cancel();
std::string tag_menu_replace_failed();
std::string tag_menu_replaced(const std::string& old_file);
std::string tag_menu_remove_prefix();
std::string tag_menu_remove_failed();
std::string tag_menu_new_name_prompt();
std::string tag_menu_new_name_empty();
std::string tag_menu_cap_prompt();
std::string tag_menu_order_prompt();
std::string tag_menu_ordered_keys_help();
std::string tag_menu_name_exists(const std::string& name);
std::string tag_menu_created(const std::string& name);
std::string tag_menu_no_deletable();
std::string tag_menu_delete_prefix();
std::string tag_menu_delete_item(int index, const std::string& name, long long tagged_count);
std::string tag_menu_delete_confirm(const std::string& name, long long count);
std::string tag_menu_deleted(const std::string& name);
std::string tag_menu_delete_failed();
std::string tag_menu_add_failed();
std::string tag_menu_main_prompt(const std::vector<pzt::core::TagSummary>& tags);

// Filter Menu
std::string filter_menu_export_prefix();
std::string filter_menu_export_current(const std::string& name);
std::string filter_menu_export_to_prompt();
std::string filter_menu_export_path_empty();
std::string filter_menu_export_io_error(const std::string& path);
std::string filter_menu_export_failed();
std::string filter_menu_export_no_images(const std::string& name);
std::string filter_menu_export_success(int count, const std::string& name, const std::string& path, bool created_folder, size_t skipped_count);
std::string filter_menu_main_prompt(const std::vector<pzt::core::TagSummary>& tags);

// Recipe Menu
std::string recipe_menu_select_preset_prefix();
std::string recipe_menu_preset_not_exist();
std::string recipe_menu_version_prompt(const std::string& preset_name);
std::string recipe_menu_version_default_label();
std::string recipe_menu_no_deletable_versions(const std::string& preset_name);
std::string recipe_menu_delete_version_prefix(const std::string& preset_name);
std::string recipe_menu_delete_failed();
std::string recipe_menu_delete_success(const std::string& name);
std::string recipe_menu_custom_full(const std::string& preset_name);
std::string recipe_menu_input_highlights();
std::string recipe_menu_input_shadows();
std::string recipe_menu_input_wb_r();
std::string recipe_menu_input_wb_b();
std::string recipe_menu_input_name();
std::string recipe_menu_create_failed();
std::string recipe_menu_create_success(const std::string& preset_name);
std::string recipe_menu_main_prompt(bool has_recipe, const std::vector<pzt::core::PresetSummary>& presets);
std::string recipe_menu_clear_failed();
std::string recipe_menu_apply_failed();
std::string recipe_menu_invalid_key();

}  // namespace pzt::cli::i18n
