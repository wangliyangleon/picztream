#pragma once

#include <cstdint>
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
// M3：`core::dedup` 用的"重复"系统标签，见 tag_display_name 的说明。
std::string duplicate_tag_label();
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
// 顶层 main() 兜底的异常边界用——core 里任何逃逸的异常(数据库 busy、磁
// 盘满、库损坏)落到这里,打一句人话再退出,而不是让 uncaught 异常直接
// terminate(那样 AltScreen/CbreakMode 的析构不会跑,终端会留在坏状态)。
std::string err_internal_error(const std::string& what);
std::string err_project_not_found(const std::string& cmd, const std::string& project_name);
std::string err_new_missing_name();
std::string err_new_name_exists(const std::string& name);
std::string err_new_no_images(const std::string& folder_path);
// F-06：`--` 开头但不是 `--support-raw` 的参数(比如拼错的
// `--supportraw`)不再被静默当成 folder_path——那样会让扫描目标变成一
// 个不存在的"目录",容易被误解成程序坏了而不是自己打错了参数。
std::string err_new_unknown_arg(const std::string& arg);
std::string msg_raw_preview_progress(int done, int total);
std::string msg_export_raw_progress(int done, int total);
std::string msg_project_created(const std::string& name, const std::string& root_path, long long image_count);
std::string msg_project_created_simple(const std::string& name);
// `pzt new` 成功之后，交互终端下追问"要不要直接打开"——见
// cli/commands/commands.cpp 的 cmd_new，非 tty(脚本调用)时不会显示这
// 条、也不会阻塞等按键。
std::string msg_new_press_any_key_to_open();
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
std::string msg_rescan_result(long long added, long long removed, long long total, long long upgraded);
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

// 右侧菜单区(下半 block)逐行显示的顶层按键提示，一行一条——只收会派生二
// 级菜单的键(' '/'x'/'g'/'e'/'r'，跟按键本身一致)，不派生二级菜单的
// h/l、j/k、q 挪到底部导航栏(见 nav_bar_text)。空行分隔符用 key=0 表
// 示、text 是空字符串——cli/commands/browse.cpp 触发某个二级菜单前,靠这
// 个 key 字段找到对应行做加粗高亮,不依赖硬编码下标。见
// cli/commands/browse.cpp 的布局说明。
struct MenuLine {
  char key;
  std::string text;
};
std::vector<MenuLine> menu_lines();
// 底部导航栏空闲时的常驻内容,分两行画(跟 space/g/r 的顶层二级菜单借用
// 同一块两行的 banner 区域):第一行 h/l、j/k,第二行 q——不这样分的话第
// 二行会一直空着,不好看。
std::string nav_bar_line1();
std::string nav_bar_line2();
std::string info_filter_label(const std::string& tag_name);
std::string info_tags_label();
std::string info_none_label();
std::string info_size_label(const std::string& size_str);
std::string info_source_label(bool is_raw);
// 标题行 + 缩进值行两行展示，见 i18n.cpp 里的说明。
std::string info_captured_at_heading();
std::string format_captured_at(std::optional<std::int64_t> captured_at);
// 标签本身("风格:"/"Recipe:")，值部分(预设名，或者没有 recipe 时复用
// info_none_label())在 browse.cpp 里跟标签拼在同一行——两部分分开是因
// 为只有值那部分需要加粗，标签本身不加粗，见 browse.cpp 的说明。
std::string info_style_label();
std::string msg_press_any_key_to_continue(const std::string& status);
std::string err_open_render_failed();
std::string err_open_decode_failed();
std::string msg_all_tagged();
std::string err_remove_tag_failed();
std::string err_filter_failed();
std::string msg_filter_no_images();
std::string msg_browse_exited();
std::string export_current_success(const std::string& output_path, bool created_folder);
std::string export_current_skipped(const std::string& file_name, pzt::core::SkipReason reason);

// M3：`:` 键触发的控制台。placeholder 按了冒号之后立刻显示，用户一开始
// 输入就整个让位给输入内容(见 read_text_line_with_placeholder)。控制台
// 现在要求所有输入必须以 `/` 开头(见 docs/M3_PRD.md"触发入口"一
// 节)——placeholder 直接把这几个命令列出来，不是笼统的"输入额外指引"。
std::string msg_ai_prompt_placeholder();
// 命名刻意不提具体能力——以后加别的能力会复用同一条"处理中"/"已提交"反
// 馈，不是新开一套文案。
std::string msg_ai_processing_pending();
std::string msg_ai_processing_submitted();
// F-03：评估请求失败(网络/key/解析，或者请求还没真正发出去就失败——
// 图片/项目找不到、预览图解码失败)之前只打 stderr，不开 --debug 时用
// 户完全看不到。poll 逻辑检测到有新结果落地时顺带查一次
// EvaluationWorker::take_last_failure()，非空就用这条文案当
// status_override 显示一次，不需要用户主动去 --debug 面板里找原因。
std::string msg_ai_evaluation_failed(pzt::core::ImageId image_id, pzt::core::EvaluationError error);
// 输入为空、或者非空但不以 `/` 开头时统一显示——控制台不再有"裸文本=当
// 前图片额外指引"这条隐藏路径，用户忘了打 `/` 不会被无声当成提交了一次
// 评估请求（这是本轮改动明确要解决的误触发风险）。Esc 依然是唯一真正的
// "取消"，这条不算取消，是"没听懂，请用 / 开头重新输入"。
std::string msg_console_requires_slash();
// `:` 输入以 `/` 开头但不是已识别的命令名时统一显示，见
// docs/M3_PRD.md"触发入口"一节——不再像最初那样静默忽略。
std::string msg_ai_unknown_command(const std::string& command);

// M3：`/dedup`、`/ai_eval` 的标签范围解析共用同一条"标签不存在"文案——
// 两边的标签范围语法都是 `#标签名`，不需要各自维护一份几乎相同的文案。
std::string err_console_tag_not_found(const std::string& tag_name);
// 范围参数既不是 `*` 也不以 `#` 开头时统一提示——不静默把它当成裸标签
// 名解析，见 docs/M3_PRD.md"触发入口"一节。
std::string err_console_invalid_scope();
// 拆成两行(跟 tag_menu_order_prompt/tag_menu_ordered_keys_help 同一个
// 先例)——prompt_and_read_key 单行版本用 pad_to 截断不换行,英文原文加上
// "(y/N)"提示很容易在正常终端宽度下被截断掉,拆成"说明"+"按键提示"两
// 行更稳妥。
std::string msg_dedup_confirm_unevaluated_line1(int unevaluated_count);
std::string msg_dedup_confirm_unevaluated_line2();
std::string msg_dedup_result(int group_count, int tagged_count);
std::string err_dedup_failed();

// M3：`/ai_eval * | #标签名 [额外指引]` 批量提交，见
// docs/M3_Eng_Design.md"`/` 命令解析"一节。count 为 0 时文案要能自然表
// 达"没有需要处理的"(比如"所有图片都已评估过")，不是"提交了 0 张"这种
// 生硬的说法。
std::string msg_ai_eval_submitted(int count);
// `/tasks`：排队中有几个、有没有正在处理中的一个，见
// core/ai/evaluation_worker.h 的 QueueStatus。
std::string msg_ai_tasks_status(std::size_t queued, bool processing);

// 还没评估过/评估失败时统一显示的占位。
std::string evaluation_none_label();
// 头部一行：综合分数(overall_score，三项平均四舍五入)+ 是否达标
// (passes_gate，三项都 >= 6)——这两个值不是模型给的，是 core::ai 算出来
// 的，见 core/ai/evaluation.h。
std::string evaluation_summary_label(int overall_score, bool passes_gate);
// 每个维度一行，note 是模型给的原因，fix 相关参数是可选的修正建议(可能
// 是 nullopt——分数已经够高、模型判断不需要修正建议时)。
std::string evaluation_exposure_line(int score, const std::string& note,
                                      std::optional<double> fix_percent);
std::string evaluation_composition_line(int score, const std::string& note,
                                         std::optional<double> rotate_degrees);
std::string evaluation_focus_line(int score, const std::string& note);

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
// space 顶层菜单拆成两行(见 prompt_and_read_key_2line):第一行带编号的
// 标签选项(0:废片 + 1-8 动态标签 + 9:重复[条件性]),第二行是固定的字
// 母操作。标签一多,单行版本会把第二行这几个操作挤到看不见的地方。
// show_duplicate 为真时在末尾追加 `9:重复`(F-01,只在项目已经存在这
// 个系统标签时才显示)。
std::string tag_menu_options_line(const std::vector<pzt::core::TagSummary>& tags,
                                   bool show_duplicate);
std::string tag_menu_actions_line();

// Filter Menu
std::string filter_menu_export_prefix();
std::string filter_menu_export_current(const std::string& name);
std::string filter_menu_export_to_prompt();
std::string filter_menu_export_path_empty();
std::string filter_menu_export_io_error(const std::string& path);
std::string filter_menu_export_failed();
std::string filter_menu_export_no_images(const std::string& name);
std::string filter_menu_export_success(int count, const std::string& name, const std::string& path, bool created_folder, size_t skipped_count);
// g 顶层菜单拆成两行,跟 tag_menu_options_line/actions_line 同样的理由。
// show_duplicate 见 tag_menu_options_line 的说明,同一条 F-01 规则。
std::string filter_menu_options_line(const std::vector<pzt::core::TagSummary>& tags,
                                      bool show_duplicate);
std::string filter_menu_actions_line();

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
// r 顶层菜单拆成两行,跟 tag_menu_options_line/actions_line 同样的理由。
std::string recipe_menu_options_line(const std::vector<pzt::core::PresetSummary>& presets);
std::string recipe_menu_actions_line(bool has_recipe);
std::string recipe_menu_clear_failed();
std::string recipe_menu_apply_failed();
std::string recipe_menu_invalid_key();

}  // namespace pzt::cli::i18n
