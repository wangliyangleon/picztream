#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "core/api.h"

namespace pzt::cli::i18n {

enum class Lang { zh, en };

extern Lang g_lang;

void init_lang();

// "废片"/reject 系统标签的显示名,单独抽出来是因为它跟其它 UI 文本不一
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
// 顶层 main() 兜底的异常边界用,core 里任何逃逸的异常(数据库 busy、磁
// 盘满、库损坏)落到这里,打一句人话再退出,而不是让 uncaught 异常直接
// terminate(那样 AltScreen/CbreakMode 的析构不会跑,终端会留在坏状态)。
std::string err_internal_error(const std::string& what);
// T-7：上面那条兜底之外唯一一个"认识的"逃逸异常。库的 schema 版本比这个
// 二进制新(装过更新版 pzt、又装回了旧版)时 core 拒绝打开并抛
// SchemaTooNewError，这里给一句能照着做的话，而不是笼统的"内部错误"。
// 不带子命令前缀：任何子命令的第一次 core 调用都可能撞上。
std::string err_db_schema_too_new(int found_version, int supported_version);
std::string err_project_not_found(const std::string& cmd, const std::string& project_name);
std::string err_new_missing_name();
std::string err_new_name_exists(const std::string& name);
std::string err_new_no_images(const std::string& folder_path);
// 目录里没有可用的 JPEG，但确实存在 RAW 文件，区别于 err_new_no_images，
// 明确告知用户 RAW 文件被 support_raw=false（默认）挡在外面，提示加
// --support-raw 重试，而不是让用户误以为目录是空的（见 T-2 proposal）。
std::string err_new_no_images_raw_ignored(const std::string& folder_path);
// F-06：`--` 开头但不是 `--support-raw` 的参数(比如拼错的
// `--supportraw`)不再被静默当成 folder_path,那样会让扫描目标变成一
// 个不存在的"目录",容易被误解成程序坏了而不是自己打错了参数。
std::string err_new_unknown_arg(const std::string& arg);
std::string msg_raw_preview_progress(int done, int total);
// T-6：分母是这一批的全部图片，不再只是其中的 RAW 张数，所以文案也不再
// 提 RAW。core 侧按整数百分比节流，一次导出最多 101 次回调。
std::string msg_export_progress(int done, int total);
std::string msg_project_created(const std::string& name, const std::string& root_path, long long image_count);
std::string msg_project_created_simple(const std::string& name);
// `pzt new` 成功之后，交互终端下追问"要不要直接打开",见
// cli/commands/commands.cpp 的 cmd_new，非 tty(脚本调用)时不会显示这
// 条、也不会阻塞等按键。
std::string msg_new_press_any_key_to_open();
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
std::string msg_project_item(const std::string& name, long long image_count, const std::string& root_path);
std::string err_rescan_missing_name();
std::string err_rescan_unknown_arg(const std::string& arg);
std::string err_rescan_failed(const std::string& name);
std::string msg_rescan_result(long long added, long long removed, long long total, long long upgraded);
std::string err_export_missing_args();
std::string err_export_unknown_arg(const std::string& arg);
std::string err_export_tag_not_found(const std::string& tag_name);
std::string err_export_io_error(const std::string& path);
std::string msg_export_no_images(const std::string& tag_name);
std::string msg_export_no_images_all();
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
std::string msg_recipe_version_item(int v, const std::string& name, double hi, double sh, double r, double b,
                                     double contrast, double saturation, double blacks, double whites);
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
// 终端可能不讲 Kitty 图像协议时的提示(T-10 (a))。在进备用屏幕之前打在真
// 实终端上,配合下面这句等一次按键 - 真机验收证明画进 banner 送不到人眼
// 前(理由见 .cpp)。不带结尾换行。
std::string warn_terminal_detail();
std::string msg_press_any_key_or_ctrl_c();

// 右侧菜单区(下半 block)逐行显示的顶层按键提示，一行一条,只收会派生二
// 级菜单的键(' '/'x'/'f'/'e'/'r'，跟按键本身一致)，不派生二级菜单的
// h/l、j/k、q 挪到底部导航栏(见 nav_bar_text)。空行分隔符用 key=0 表
// 示、text 是空字符串,cli/commands/browse.cpp 触发某个二级菜单前,靠这
// 个 key 字段找到对应行做加粗高亮,不依赖硬编码下标。见
// cli/commands/browse.cpp 的布局说明。
struct MenuLine {
  char key;
  std::string text;
};
std::vector<MenuLine> menu_lines();
// 底部导航栏空闲时的常驻内容,分两行画(跟 space/g/r 的顶层二级菜单借用
// 同一块两行的 banner 区域):第一行 h/l、j/k,第二行 q,不这样分的话第
// 二行会一直空着,不好看。
std::string nav_bar_line1();
std::string nav_bar_line2();
// f 层标签筛选/控制台二级筛选各自可能生效，也可能同时生效,统一拼成
// "TagName | criterion" 这种不带标签前缀的紧凑写法，都不生效时返回空
// 串。console_criterion_keyword 是 "unevaluated"/"fail"/"reject"/"dup"
// 之一(跟解析 `/filter <criterion>` 用的是同一套关键字)，这个函数不
// 需要认识 cli 内部的 ConsoleFilterCriterion 枚举类型，只接字符串。
std::string info_active_filters_label(const std::optional<std::string>& tag_name,
                                       const std::optional<std::string>& console_criterion_keyword);
std::string info_tags_label();
std::string info_none_label();
std::string info_size_label(const std::string& size_str);
std::string info_source_label(bool is_raw);
// 标题行 + 缩进值行两行展示，见 i18n.cpp 里的说明。
std::string info_captured_at_heading();
std::string format_captured_at(std::optional<std::int64_t> captured_at);
// 标签本身("配方:"/"Recipe:")，值部分(预设名，或者没有 recipe 时复用
// info_none_label())在 browse.cpp 里跟标签拼在同一行,两部分分开是因
// 为只有值那部分需要加粗，标签本身不加粗，见 browse.cpp 的说明。
std::string info_style_label();
std::string msg_press_any_key_to_continue(const std::string& status);
std::string err_open_render_failed();
std::string err_open_decode_failed();
std::string msg_all_tagged();
std::string err_remove_tag_failed();
std::string err_filter_failed();
std::string msg_filter_no_images();
// F-09：`/filter <criterion>` 计算结果为空时显示,跟 msg_filter_no_images
// 分开，那条是"这个标签下没有图片"(标签语义)，这条是"没有满足这个
// 状态条件的图片"(评估/去重状态语义)。
std::string msg_console_filter_no_images();
std::string msg_browse_exited();
std::string export_current_success(const std::string& output_path, bool created_folder);
std::string export_current_skipped(const std::string& file_name, pzt::core::SkipReason reason);

// M3：`:` 键触发的控制台。placeholder 按了冒号之后立刻显示，用户一开始
// 输入就整个让位给输入内容(见 read_text_line_with_placeholder)。控制台
// 现在要求所有输入必须以 `/` 开头(见 docs/history/M3_PRD.md"触发入口"一
// 节),placeholder 直接把这几个命令列出来，不是笼统的"输入额外指引"。
std::string msg_ai_prompt_placeholder();
// 命名刻意不提具体能力,以后加别的能力会复用同一条"处理中"/"已提交"反
// 馈，不是新开一套文案。
std::string msg_ai_processing_pending();
std::string msg_ai_processing_submitted();
// F-03：评估请求失败(网络/key/解析，或者请求还没真正发出去就失败,
// 图片/项目找不到、预览图解码失败)之前只打 stderr，不开 --debug 时用
// 户完全看不到。poll 逻辑检测到有新结果落地时顺带查一次
// EvaluationWorker::take_failure_report()，非空就用这条文案当
// status_override 显示一次，不需要用户主动去 --debug 面板里找原因。
// T-23：两个参数是那次改动加的。file_name 优先于 image_id - 用户手上
// 只有文件名，界面其它每一处也都用 file_name 展示(browse.cpp 的信息
// 栏)，裸数据库 ID 无从对照；查不到记录(比如失败原因本身就是"图片记录
// 找不到")时才回落到 ID，那总比整条提示消失强。total_failed 是本次
// `pzt open` 累计失败的张数，>1 时文案要报出这个数字：批量评估的失败
// 高度相关(key 没配、Ollama 没起来，一挂就是整批)，只报最近一条会让用
// 户以为只错了一张，那个数字才是"值得停下来检查环境"的信号。
std::string msg_ai_evaluation_failed(const std::optional<std::string>& file_name,
                                      pzt::core::ImageId image_id,
                                      pzt::core::EvaluationError error, std::size_t total_failed);
// 输入为空、或者非空但不以 `/` 开头时统一显示,控制台不再有"裸文本=当
// 前图片额外指引"这条隐藏路径，用户忘了打 `/` 不会被无声当成提交了一次
// 评估请求（这是本轮改动明确要解决的误触发风险）。Esc 依然是唯一真正的
// "取消"，这条不算取消，是"没听懂，请用 / 开头重新输入"。
std::string msg_console_requires_slash();
// `:` 输入以 `/` 开头但不是已识别的命令名时统一显示，见
// docs/history/M3_PRD.md"触发入口"一节,不再像最初那样静默忽略。
std::string msg_ai_unknown_command(const std::string& command);

// `/help`：不带参数列出全部命令名，带参数(命令名不含前导 `/`)显示这个
// 命令的详细用法。msg_help_command 返回 nullopt 表示这个命令名没有对
// 应的帮助条目，调用方据此改用 err_help_unknown_command。
std::string msg_help_overview();
std::optional<std::string> msg_help_command(const std::string& command);
std::string err_help_unknown_command(const std::string& command);

// M3：`/dedup`、`/ai_eval` 的标签范围解析共用同一条"标签不存在"文案,
// 两边的标签范围语法都是 `#标签名`，不需要各自维护一份几乎相同的文案。
std::string err_console_tag_not_found(const std::string& tag_name);
// 范围参数既不是 `*` 也不以 `#` 开头时统一提示,不静默把它当成裸标签
// 名解析，见 docs/history/M3_PRD.md"触发入口"一节。
std::string err_console_invalid_scope();
// T-16/#27：`/dedup` 的范围本身是系统标签(废片/重复)时拒绝。此前是静
// 默 no-op——范围被正确解析出来、进 core 后全被排除，命令报"0 组",用
// 户无从分辨这是"真没有重复"还是"范围被清空了"。参数收的是库里的
// canonical 名,显示名由这个函数按当前语言换算。
std::string err_console_dedup_system_tag_scope(const std::string& canonical_tag_name);
// T-15（#30）：范围写的是 `.`（当前视图）但这条路径没有视图可传。`.` 本
// 身是合法写法，所以不能复用 err_console_invalid_scope 那句"必须是 * 或
// #标签名" - 那是假话。控制台的 `/dedup`/`/ai_eval` 接上视图之后（票 D）
// 这一支在交互层不再出现，映射仍然要在，因为 core 的错误集合是穷举的。
std::string err_console_scope_no_view();
// F-09：`/filter` 的 criterion 参数缺失或不是词汇表里的四个词之一时提
// 示,控制台一贯"显式标记，不猜"的风格，不静默忽略、不模糊匹配。
std::string err_console_invalid_filter_criterion();
// 反馈:退出时如果还有评估任务排队/处理中，队列里还没开始处理的部分
// 会被直接丢弃(EvaluationWorker 析构只等当前正在处理的这一个，不会
// 继续消费队列剩下的)，静默退出容易让用户以为提交的一批评估都在跑，
// 其实中途被打断了一部分,加一次确认，给反悔机会。
std::string msg_quit_confirm_pending_line1(int pending_count);
std::string msg_quit_confirm_pending_line2();
// F-08：skipped_no_capture_time 是范围内因为没有拍摄时间(captured_at
// 为 NULL)完全没参与比较的图片数,>0 时带一句提示,不静默排除。
// ai_fallback_count 同理:>0 时说明这次有几组的保留项其实是 AI 比较失败
// 后按拍摄时间兜底选的,不开 --ai 时恒为 0、不出现在文案里。
std::string msg_dedup_result(int group_count, int tagged_count, int skipped_no_capture_time,
                              int ai_fallback_count);
std::string err_dedup_failed();
// `/dedup` 阻塞期间的两段进度:先是本地分组(纯 CPU,秒级),开了 --ai 之
// 后还有 AI 逐组比较(网络,分钟级)。两段分别计数、total 对不上,所以是
// 两个文案而不是一个,见 core/tournament/tournament.h 的 AiProgressFn。
std::string msg_dedup_cluster_progress(int done, int total);
// AI 那段带两级计数:只报组号的话,单个大簇期间(一簇就是 size-1 次串行网
// 络调用)画面会静止几分钟,见 AiProgressFn 的说明。比较次数的分母跟
// msg_dedup_ai_confirm_line1 报给用户的是同一个数。
std::string msg_dedup_ai_progress(int group_done, int group_total, int comparison_done,
                                   int comparison_total);
// `/dedup` 阻塞期间挂在 banner 第二行的操作提示,整条命令期间都在(不只是
// AI 段——分簇阶段同样可取消)。T-9b 之后 Ctrl-C 真的只取消这一次 dedup、
// 不退出 pzt open,所以这里就写"取消";在那之前它是强杀,文案得额外说明会
// 退出整个程序。
std::string msg_dedup_ai_progress_hint();
// 按下 Ctrl-C 的那一刻立刻回显的一行。**这行字由信号处理函数直接 write()
// 出去**——按下时主线程正阻塞在网络请求或解码里,没人能替它重画。所以它
// 必须在开跑前就渲染好(见 handle_dedup_command),处理函数里不能再调
// i18n。取消最长要等当前这一次比较跑完,没有这行回显的话用户会以为没按
// 上、反复按。
std::string msg_dedup_cancelling();
// 取消真正生效之后的结果行。跟"闸门被拒"分开报:那个是"没点头",这个是
// "点了头又喊停"——后者已经花掉了时间和 token,用户需要知道这次是白跑
// 的,而不是以为自己从来没启动过。
std::string msg_dedup_cancelled();
// `--ai` 真正开跑前的开销确认。拆成两行跟 msg_quit_confirm_pending_*
// 同一个先例:prompt_and_read_key 单行版本用 pad_to 截断不换行,英文文案
// 再加上按键提示很容易在正常终端宽度下被截掉,拆成"说明"+"按键提示"两
// 行更稳妥。comparison_count 是精确值不是估算,见 AiGateFn 的说明。
std::string msg_dedup_ai_confirm_line1(int group_count, int comparison_count);
std::string msg_dedup_ai_confirm_line2();
// `/dedup` 范围之后跟了除 `--ai` 以外的东西。控制台一贯"显式标记，不
// 猜"的风格,不把认不出来的 token 当标签名吞掉,跟
// err_console_invalid_filter_criterion 同一个理由。
std::string err_dedup_bad_args();

// M3：`/ai_eval * | #标签名 [额外指引]` 批量提交，见
// docs/history/M3_Eng_Design.md"`/` 命令解析"一节。count 为 0 时文案要能自然表
// 达"没有需要处理的"(比如"所有图片都已评估过")，不是"提交了 0 张"这种
// 生硬的说法。
std::string msg_ai_eval_submitted(int count);
// `/tasks`：排队中有几个、有没有正在处理中的一个，见
// core/ai/evaluation_worker.h 的 QueueStatus。
// T-23：failed 是这一次 `pzt open` 里累计失败的张数,0 时整句不提失败
// (没挂过就别引入这个概念)。失败的状态行是一次性的、错过就没了，这里
// 是唯一能事后回看"到底挂了多少张"的地方。
std::string msg_ai_tasks_status(std::size_t queued, bool processing, std::size_t failed);

// 还没评估过/评估失败时统一显示的占位。
std::string evaluation_none_label();
// W2026-07-21：AI 点评区块的标题行。刻意不叫"选片/Culling",那会跟 agent
// 的选片功能混淆。assessment 文字本身由 core 直接给，不经 i18n(是模型输
// 出，不是 UI 文案)。
std::string evaluation_comment_label();
// 只有 unusable(有硬伤)时才显示的一行，在 assessment 之前加粗,可用时什
// 么都不显示。
std::string evaluation_unusable_label();

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
// T-24：动态标签数已达菜单上限时,`space c` 直接被挡住,这句话说明上限是
// 多少、以及怎么才能继续建。
std::string tag_menu_limit_reached(int limit);
// space 顶层菜单拆成两行(见 prompt_and_read_key_2line):第一行带编号的
// 标签选项(0:废片 + 1-8 动态标签 + 9:重复[条件性]),第二行是固定的字
// 母操作。标签一多,单行版本会把第二行这几个操作挤到看不见的地方。
// show_duplicate 为真时在末尾追加 `9:重复`(F-01,只在项目已经存在这
// 个系统标签时才显示)。
std::string tag_menu_options_line(const std::vector<pzt::core::TagSummary>& tags,
                                   bool show_duplicate);
// T-24：at_limit 为真时给 `c` 加一个"已满"标记,让用户在按下去之前就知道
// 建不了。选项本身不拿掉-拿掉的话,按 c 得到一句解释这条路径也没了。
// hidden 是超出菜单上限、编号行里选不到的标签数量(老项目可能在"建到上限
// 就挡住"之前已经建了 8 个以上),非 0 时在行尾标出来。这两条注记都挂在
// 操作行而不是编号行:编号行排满 8 个标签就要 100 列开外,而 content_cols
// 只有终端宽度的 70%,pad_to 从尾部截-挂在那一行的话,恰恰在标签最多、
// 最该提示的时候第一个被切掉。操作行固定三四个字母选项,不会溢出。
std::string tag_menu_actions_line(bool at_limit, std::size_t hidden);

// Filter Menu
// `e` 键的二级子菜单提示,始终弹出，filter_active 只决定要不要把
// `f`(导出筛选结果)这一项拼进去，见 browse.cpp 顶层 `e` 键处理的说明。
std::string msg_export_submenu_prompt(bool filter_active);
std::string filter_menu_export_to_prompt();
std::string filter_menu_export_path_empty();
std::string filter_menu_export_io_error(const std::string& path);
std::string filter_menu_export_failed();
std::string filter_menu_export_no_images();
std::string filter_menu_export_success(int count, const std::string& path, bool created_folder, size_t skipped_count);
// g 顶层菜单拆成两行,跟 tag_menu_options_line/actions_line 同样的理由。
// show_duplicate 见 tag_menu_options_line 的说明,同一条 F-01 规则。
std::string filter_menu_options_line(const std::vector<pzt::core::TagSummary>& tags,
                                      bool show_duplicate);
// hidden 见 tag_menu_actions_line 的说明,两个菜单共用同一套编号、被截断的
// 也是同一批标签,注记同样挂在不会溢出的操作行。
std::string filter_menu_actions_line(std::size_t hidden);

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
std::string recipe_menu_input_contrast();
std::string recipe_menu_input_saturation();
std::string recipe_menu_input_blacks();
std::string recipe_menu_input_whites();
std::string recipe_menu_input_name();
// issue #19：`r c` 分步向导每个字段提示的前缀:第几步/共几步，以及(非首
// 个字段上)Backspace 能回退这件事:回退没有任何视觉痕迹,不写在提示里用
// 户不会知道它存在。前缀之后紧跟 recipe_menu_input_* 那几条(它们各自自
// 带前导空格)。
std::string recipe_menu_wizard_step_prefix(std::size_t step, std::size_t total, bool can_go_back);
std::string recipe_menu_create_failed();
std::string recipe_menu_create_success(const std::string& preset_name);
// r 顶层菜单的操作图例(r/c/d/v/esc)。编号选项的铺行由 cli/menu/recipe_menu
// 的 build_recipe_menu_lines 负责(要按显示宽度铺满两行、图例右对齐)，i18n
// 只提供图例这段本地化文案。
std::string recipe_menu_actions_line(bool has_recipe);
// T-15 票 C：批量套配方那个菜单的操作图例。跟单张那份不同构(没有
// has_recipe、也没有 v/c/d)，所以是独立一条而不是给上面那个加参数 - 决策
// D-14 定的是"批量菜单是单张菜单的子集"，两份图例各自成立。
std::string recipe_menu_actions_line_batch();
// 把 describe_recipe 的 {预设名, 可选 version 名} 合成 banner 用的单行显
// 示名。信息栏分两行显示同一件事，banner 只有一行。
std::string recipe_display_name(const std::string& preset_name,
                                const std::optional<std::string>& version_name);
// T-15 票 C 决策 D-9：批量套配方/批量清除前**总是**弹的两行确认。total 是
// 作用域内张数 N，overwritten 是其中原本已有配方、会被覆盖且无法还原的张数
// M - M 是文案主角。recipe_name 为空表示这次是批量清除。
std::string msg_recipe_batch_confirm_line1(int total, int overwritten,
                                            const std::optional<std::string>& recipe_name);
std::string msg_recipe_batch_confirm_line2();
// 决策 D-15：闪 800ms 的回执，不占额外按键。
std::string msg_recipe_batch_applied(int total, const std::string& recipe_name);
std::string msg_recipe_batch_cleared(int total);
std::string msg_recipe_scope_no_images();
std::string err_recipe_bad_args();
std::string msg_recipe_batch_failed();
std::string recipe_menu_clear_failed();
std::string recipe_menu_apply_failed();
std::string recipe_menu_invalid_key();
// T-3：顶层浏览循环按到不支持的键时的提示,带上按的是哪个键。二级菜单层
// 早有"无效按键给提示"的约定,只有顶层是完全静默的。
std::string msg_unknown_key(char key);

}  // namespace pzt::cli::i18n
