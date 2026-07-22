#include "cli/commands/commands.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "cli/kitty/kitty.h"
#include "cli/menu/filter_menu.h"
#include "cli/menu/recipe_menu.h"
#include "cli/menu/tag_menu.h"
#include "cli/term/cbreak_mode.h"
#include "cli/term/debug_log.h"
#include "cli/term/screen.h"
#include "cli/text/text.h"
#include "cli/ui/ui.h"
#include "cli/i18n/i18n.h"
#include "core/api.h"

// 搬过来的 cmd_open 函数体调用了 cli/text、cli/ui、cli/menu 里的一大堆函
// 数(pad_to/move_cursor/draw_hline/tags_for_menu/handle_space_key 等),
// 用 using-directive 让函数体保持逐字不变(.cpp 里用 using,头文件里绝不
// 用)。
using namespace pzt::cli::text;
using namespace pzt::cli::ui;
using namespace pzt::cli::menu;

namespace pzt::cli::commands {
namespace {

// 切换浏览池子(应用筛选/清除筛选)后 current_id 该是谁:能留在原地就留在
// 原地(原来那张图还在新池子里),留不住就退回列表头。两个方向复用同一条
// 规则,不为"进筛选"和"出筛选"分别定义两套语义。
pzt::core::ImageId resolve_current_after_switch(const std::vector<pzt::core::ImageRef>& new_images,
                                                 pzt::core::ImageId desired) {
  for (const auto& ref : new_images) {
    if (ref.id == desired) return desired;
  }
  return new_images.front().id;
}

// F-10：AI 供应商固定写死 Gemini 只是因为开发时手头只有 Gemini 的
// key，不是经过设计的选择(docs/M3_PRD.md"风险与待确认问题"一节的
// TODO 原话)。2026-07-22：去掉 PZT_AI_PROVIDER 环境变量覆盖——跟 lang
// 同样的理由(见 init_lang 的注释)，只留 F-12 的 Settings.ai_provider
// (config.json)一个来源，config 没写就用 Settings 自己的默认值 Local，
// 不必再猜"这次生效的是环境变量还是配置文件"。不缓存，每次调用现读——
// 这不是热路径(只在用户真的提交一次 /ai_eval 相关命令时才会走到)，
// config.json 是廉价操作，不值得为了省这几次调用专门传参或加个全局变
// 量；现读还有个好处:用户中途改了 config.json 不需要重启 pzt open。
pzt::core::Provider resolve_ai_provider() { return pzt::core::load_settings().ai_provider; }

// 当前界面语言映射成 core 的 assessment 语言(eval 的 guidance 为空时用
// 它,见 core/ai/evaluation.h 的 Language)。cli 决定语言,core 不认识 i18n。
pzt::core::Language resolve_assessment_language() {
  return pzt::cli::i18n::g_lang == pzt::cli::i18n::Lang::en ? pzt::core::Language::English
                                                            : pzt::core::Language::Chinese;
}

// Provider::Local 的连接信息——跟 resolve_ai_provider() 同一个"现读不
// 缓存"惯例，用户中途改了 config.json 不需要重启 pzt open。
pzt::core::LocalModelConfig resolve_local_model_config() {
  auto settings = pzt::core::load_settings();
  return pzt::core::LocalModelConfig{settings.ollama_base_url, settings.ollama_model};
}

// 信息栏"标签:"这一行——原来一行一个标签太占竖直空间，改成
// "标签: #A #B" 全挤在一行，宽度不够再换行。带空格的标签名用引号包起
// 来(`#"Some Other"`)，不然拆词时会被误判成两个 token。
std::string tag_token(const pzt::core::TagSummary& t) {
  std::string name = pzt::cli::i18n::tag_display_name(t);
  if (name.find(' ') != std::string::npos) return "#\"" + name + "\"";
  return "#" + name;
}

// 顶层 `e` 键:直接导出当前正在看的这一张,不需要先建标签。流程照抄
// filter_menu.cpp 里 handle_g_export_flow 的结构(读路径 -> 校验空 ->
// expand_home_path -> 调导出 -> 拼状态文案),但进度回调不能用 cmd_export
// 那套 \r 覆写 stdout 的写法——这里在 AltScreen 里跑固定坐标布局,直接写
// stdout 会破坏画面,得跟 banner 其它内容一样走 move_cursor + pad_to +
// write_stdout。
std::string handle_export_current_flow(pzt::core::ImageId image_id, const std::string& file_name,
                                        int banner_row, int start_col, int content_cols) {
  auto path = read_text_line(pzt::cli::i18n::filter_menu_export_to_prompt(), banner_row, start_col,
                              content_cols);
  if (!path) return "";  // Esc,静默取消
  if (path->empty()) return pzt::cli::i18n::filter_menu_export_path_empty();
  std::string resolved_path = expand_home_path(*path);

  auto on_progress = [&](int done, int total) {
    move_cursor(banner_row, start_col + 1);
    write_stdout(pad_to(pzt::cli::i18n::msg_export_raw_progress(done, total), content_cols));
  };
  auto result = pzt::core::export_image(image_id, resolved_path, on_progress);
  // F-25：单张 RAW 全量解码是秒级耗时，同样可能冻结主循环——见
  // handle_dedup_command 里同一处修复的说明。
  flush_pending_input();
  if (!result.ok()) {
    if (result.error() == pzt::core::ExportImageError::IoError) {
      return pzt::cli::i18n::filter_menu_export_io_error(resolved_path);
    }
    return pzt::cli::i18n::filter_menu_export_failed();
  }

  const auto& r = result.value();
  if (!r.exported) return pzt::cli::i18n::export_current_skipped(file_name, *r.skip_reason);
  return pzt::cli::i18n::export_current_success(r.output_path, r.created_output_folder);
}

// 点 2：`e` 二级菜单里的 `f`——导出当前 active filter 范围(g 层 ∘ 二级
// 筛选叠加之后 cmd_open 手上的 images，不是某个具体标签)。include_
// reject/include_dup 由调用方(cmd_open)算好传进来——"当前筛选本身就
// 是废片/重复"这个对称例外只有调用方知道(既可能来自 g 层标签、也可
// 能来自控制台二级筛选criterion)，这个函数不重新判断。
std::string handle_export_filtered_flow(pzt::core::ProjectId project_id,
                                         const std::vector<pzt::core::ImageRef>& images,
                                         bool include_reject, bool include_dup, int banner_row,
                                         int start_col, int content_cols) {
  auto path = read_text_line(pzt::cli::i18n::filter_menu_export_to_prompt(), banner_row, start_col,
                              content_cols);
  if (!path) return "";  // Esc,静默取消
  if (path->empty()) return pzt::cli::i18n::filter_menu_export_path_empty();
  std::string resolved_path = expand_home_path(*path);

  auto on_progress = [&](int done, int total) {
    move_cursor(banner_row, start_col + 1);
    write_stdout(pad_to(pzt::cli::i18n::msg_export_raw_progress(done, total), content_cols));
  };
  std::vector<pzt::core::ImageId> ids;
  ids.reserve(images.size());
  for (const auto& ref : images) ids.push_back(ref.id);
  auto result = pzt::core::export_images(project_id, ids, resolved_path, on_progress, include_reject,
                                          include_dup);
  // F-25：大批量导出(尤其是带 RAW 图片的批次)可能冻结主循环几秒到几十
  // 秒——见 handle_dedup_command 里同一处修复的说明。
  flush_pending_input();
  if (!result.ok()) {
    return pzt::cli::i18n::filter_menu_export_io_error(resolved_path);  // 唯一的失败原因就是 IoError
  }

  const auto& r = result.value();
  if (r.exported_count == 0 && r.skipped.empty()) {
    return pzt::cli::i18n::filter_menu_export_no_images();
  }
  return pzt::cli::i18n::filter_menu_export_success(r.exported_count, resolved_path,
                                                     r.created_output_folder, r.skipped.size());
}

// ASCII 大小写不敏感比较——只用来判断"这段英文是不是 Reject/Duplicate
// 的某种大小写拼法"，跟 core::tagging 里 COLLATE NOCASE 是同一个不敏感
// 范围(只影响 A-Z/a-z，中文不受影响)，这里不复用那条 SQL 路径是因为
// 比较的是常量字符串，不需要真的去查库。
bool equals_ascii_case_insensitive(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

// 系统标签(废片/重复)在数据库里永远存中文名(见 kRejectTagName/
// kDuplicateTagName 的说明)，但展示层的名字跟着当前 UI 语言走——英文
// 界面下用户在信息栏/菜单里看到的是"Reject"/"Duplicate"，很自然地会
// 拿这个词去打 `#Reject`(或者 `#REJECT`/`#reject`，跟标签大小写不敏
// 感是同一个便利性诉求)，如果只按存库的中文名精确匹配就会得到"标签
// 不存在"，语言相关的行为反而成了 bug。这里两种拼法(含任意大小写)都
// 认，不管当前 g_lang 是什么；普通用户自己建的标签不受这条特判影响，
// 仍然走 find_tag_by_name 本身的(大小写不敏感、但不做语言别名)匹配。
std::optional<pzt::core::TagId> resolve_tag_name_language_independent(pzt::core::ProjectId project_id,
                                                                       const std::string& name) {
  if (name == pzt::core::tagging::kRejectTagName || equals_ascii_case_insensitive(name, "Reject")) {
    return pzt::core::find_tag_by_name(project_id, pzt::core::tagging::kRejectTagName);
  }
  if (name == pzt::core::tagging::kDuplicateTagName ||
      equals_ascii_case_insensitive(name, "Duplicate")) {
    return pzt::core::find_tag_by_name(project_id, pzt::core::tagging::kDuplicateTagName);
  }
  return pzt::core::find_tag_by_name(project_id, name);
}

// `/dedup`/`/ai_eval` 共用的批量范围解析：`*` 整个项目、`#标签名` 带指
// 定标签的图片，标签名带空格时用 `#"标签名"` 包起来——两边统一用同一
// 套写法，不各自维护一套解析和错误文案。scope 不是 `*` 也不以 `#` 开
// 头时，error_message 给一条"范围写法不对"的提示，不静默当成标签名。
struct ScopeResolution {
  std::vector<pzt::core::ImageId> image_ids;
  std::string error_message;  // 非空表示解析失败，caller 直接把它当结果返回
  // F-26：范围标签本身的 id，`*` 时为空。用来判断"范围本身就是废片/
  // 重复"这个对称例外——这种情况下用户已经显式要求处理它，不再排除。
  std::optional<pzt::core::TagId> scope_tag_id;
};

ScopeResolution resolve_console_scope(pzt::core::ProjectId project_id, const std::string& scope) {
  ScopeResolution result;
  if (scope == "*") {
    for (const auto& ref : pzt::core::list_images(project_id)) result.image_ids.push_back(ref.id);
    return result;
  }
  if (scope.empty() || scope[0] != '#') {
    result.error_message = pzt::cli::i18n::err_console_invalid_scope();
    return result;
  }
  std::string tag_name = scope.substr(1);
  if (tag_name.size() >= 2 && tag_name.front() == '"' && tag_name.back() == '"') {
    tag_name = tag_name.substr(1, tag_name.size() - 2);
  }
  auto tag_id = resolve_tag_name_language_independent(project_id, tag_name);
  if (!tag_id) {
    result.error_message = pzt::cli::i18n::err_console_tag_not_found(tag_name);
    return result;
  }
  result.scope_tag_id = *tag_id;
  auto filtered = pzt::core::filter_by_tag(*tag_id);
  if (!filtered.ok()) {
    result.error_message = pzt::cli::i18n::err_filter_failed();
    return result;
  }
  for (const auto& ref : filtered.value()) result.image_ids.push_back(ref.id);
  return result;
}

// F-26：从 resolved.image_ids 里剔除带 reject_tag_id 这个标签的图片，除
// 非范围本身就是这个标签(对称例外)——eval/dedup 的批量范围各自受一个
// 独立开关控制(settings.eval_reject/dedup_reject)，共用这一份过滤逻
// 辑。reject_tag_id 为空(项目里还没有对应系统标签)时直接跳过，不当错
// 误处理。
void exclude_scope_by_tag(ScopeResolution& resolved, std::optional<pzt::core::TagId> exclude_tag_id) {
  if (!exclude_tag_id || resolved.scope_tag_id == *exclude_tag_id) return;
  auto matched = pzt::core::images_with_tag(resolved.image_ids, *exclude_tag_id);
  if (matched.empty()) return;
  auto& ids = resolved.image_ids;
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](auto id) { return matched.count(id) > 0; }),
            ids.end());
}

// `/dedup * | #标签名`——近似重复检测唯一的触发入口，见
// docs/M3_Dedup_Eng_Design.md"控制台命令"一节。范围内有图片还没跑过选
// 片辅助评估时先问一句 y/N——不代为触发评估，只是提醒"这次的保留判断可
// 能会退化成按拍摄时间选"；按其它任何键(含 Esc)都算取消，跟
// tag_menu.cpp 里"是否有序"那个 y/N 确认同一个约定。真正比对这一步是
// 阻塞的:find_and_tag_duplicates 内部可能跑几秒到几十秒，这段时间
// pzt open 冻结、不接受任何输入——刻意的简化，见
// docs/M3_Dedup_PRD.md"非目标"一节，不传 on_progress 是因为这段时间主
// 循环没有机会重绘，传了也没地方画。
std::string handle_dedup_command(pzt::core::ProjectId project_id, const std::string& scope,
                                  int banner_row, int start_col, int content_cols) {
  auto resolved = resolve_console_scope(project_id, scope);
  if (!resolved.error_message.empty()) return resolved.error_message;

  // F-12/F-26：一次读全,时间窗/哈希阈值(F-08)和废片排除开关都来自
  // 同一份 Settings,现读不缓存,跟 resolve_ai_provider() 同一个先例。
  auto settings = pzt::core::load_settings();
  if (!settings.dedup_reject) {
    exclude_scope_by_tag(resolved,
                          pzt::core::find_tag_by_name(project_id, pzt::core::tagging::kRejectTagName));
  }

  // F-07：以前逐张 get_image() 判断评估状态，大项目按一次键就是几百到
  // 几千次数据库往返。改成一条批量查询，只统计数量。
  auto evaluated = pzt::core::evaluated_image_ids(resolved.image_ids);
  int unevaluated = static_cast<int>(resolved.image_ids.size()) - static_cast<int>(evaluated.size());
  if (unevaluated > 0) {
    // 拆两行,跟 tag_menu.cpp 里"是否有序"那个确认同一个先例——见
    // msg_dedup_confirm_unevaluated_line1/2 的说明。
    char c = prompt_and_read_key_2line(pzt::cli::i18n::msg_dedup_confirm_unevaluated_line1(unevaluated),
                                        pzt::cli::i18n::msg_dedup_confirm_unevaluated_line2(),
                                        banner_row, start_col, content_cols);
    if (c != 'y' && c != 'Y') return "";  // 取消,静默
  }

  auto result = pzt::core::find_and_tag_duplicates(project_id, resolved.image_ids,
                                                     settings.dedup_time_window_seconds,
                                                     settings.dedup_hash_threshold, /*on_progress=*/nullptr);
  // F-25：这一步可能冻结了几秒到几十秒，期间用户习惯性按的键留在 tty
  // 缓冲区里——不清掉的话，接下来继续读键时会一次性回放，可能连按出
  // 误标签/误退出。见 docs/M3_Dedup_PRD.md"阻塞期间的输入缓冲行为"那
  // 条一直没收口的风险。
  flush_pending_input();
  if (!result.ok()) return pzt::cli::i18n::err_dedup_failed();
  return pzt::cli::i18n::msg_dedup_result(result.value().group_count, result.value().tagged_count,
                                           result.value().skipped_no_capture_time);
}

// `/ai_eval * | #标签名 [额外指引]`——批量提交，见
// docs/M3_PRD.md"批量评估与任务状态"一节。已经评估过的直接跳过，不重
// 新评估（哪怕这次带了不同的额外指引）；单张重新评估只能走
// `/ai_eval [额外指引]`(当前图片)那条路径，逐张手动做。提交立即返回，
// 不等这一批全部完成——`request()` 本身的去重(`in_flight_`)保证批量提
// 交跟单张手动触发不会互相冲突，不需要在这里额外处理。
std::string handle_ai_eval_command(pzt::core::EvaluationWorker& evaluation_worker,
                                    pzt::core::ProjectId project_id, const std::string& scope,
                                    const std::string& extra_guidance) {
  auto resolved = resolve_console_scope(project_id, scope);
  if (!resolved.error_message.empty()) return resolved.error_message;

  // F-26：同上，默认排除废片，除非范围本身就是 #废片。
  if (!pzt::core::load_settings().eval_reject) {
    exclude_scope_by_tag(resolved,
                          pzt::core::find_tag_by_name(project_id, pzt::core::tagging::kRejectTagName));
  }

  // F-07：同上，一条批量查询代替逐张 get_image()。
  auto evaluated = pzt::core::evaluated_image_ids(resolved.image_ids);
  // M4：auto_reject 现在是 request() 的显式参数(见 evaluation_worker.h)，
  // 交互路径在这里读一次 Settings 透传，行为跟以前完全一样，只是读取
  // 点从 worker 内部挪到了提交侧。
  bool auto_reject = pzt::core::load_settings().auto_ai_reject;
  int submitted = 0;
  for (auto id : resolved.image_ids) {
    if (evaluated.count(id)) continue;  // 已经评估过,跳过
    if (evaluation_worker.request(id, resolve_ai_provider(), extra_guidance, auto_reject,
                                   resolve_assessment_language(), resolve_local_model_config()))
      ++submitted;
  }
  return pzt::cli::i18n::msg_ai_eval_submitted(submitted);
}

// `/tasks`——查看评估队列的状态，不需要参数。
std::string handle_tasks_command(pzt::core::EvaluationWorker& evaluation_worker) {
  auto status = evaluation_worker.queue_status();
  return pzt::cli::i18n::msg_ai_tasks_status(status.queued, status.processing);
}

// F-09：控制台 `/filter <criterion>` 二级筛选——在当前 g 筛选结果之上
// (没有 g 筛选时就是全项目)再筛一层，不是 g 菜单的第三种选项，可以
// 跟 g 标签筛选同时生效。词汇表(拍板已定):未评估/评估不达标/废片/重
// 复，不做 `/sort`/`/reject_failed` 这类原方案里被否掉的其它变体。
enum class ConsoleFilterCriterion { Unevaluated, Fail, Reject, Dup };

// `handle_ai_console_command` 原来只需要"发起动作、报个状态"，返回纯
// `std::string` 就够；`/filter` 要改 cmd_open 主循环的浏览池状态，所
// 以分发层的返回类型升级成这个小结构体。其它命令分支只填 status，
// action 留默认 NoChange，行为跟以前完全一样。
struct ConsoleCommandResult {
  std::string status;
  enum class FilterAction { NoChange, Clear, Apply } action = FilterAction::NoChange;
  ConsoleFilterCriterion criterion{};  // 仅 action == Apply 时有意义
};

// 把 ConsoleFilterCriterion 转回控制台原本的关键字——info_console_filter_label
// 之类的 i18n 函数只接字符串，不需要认识这个 cli 内部枚举类型。
const char* console_filter_criterion_keyword(ConsoleFilterCriterion criterion) {
  switch (criterion) {
    case ConsoleFilterCriterion::Unevaluated:
      return "unevaluated";
    case ConsoleFilterCriterion::Fail:
      return "fail";
    case ConsoleFilterCriterion::Reject:
      return "reject";
    case ConsoleFilterCriterion::Dup:
      return "dup";
  }
  return "";
}

// `/filter` 真正的筛选计算——只在 cmd_open 收到 Apply 意图之后才调用
// (跟 g 键"handle_g_key_prompt 只返回意图，cmd_open 自己算"同一个既
// 有模式)，base 是当前 g 层的结果(cmd_open 的 g_filtered_images)。
// reject/dup 复用 F-26 的 images_with_tag(一条查询)；unevaluated/fail
// 逐张 get_image() 判断——已知 N+1，量级跟 handle_ai_eval_command 现
// 有实现一致，这轮不顺带优化(那是 F-07 的范围)。
std::vector<pzt::core::ImageRef> apply_console_filter(pzt::core::ProjectId project_id,
                                                       const std::vector<pzt::core::ImageRef>& base,
                                                       pzt::core::TagId reject_tag_id,
                                                       ConsoleFilterCriterion criterion) {
  std::vector<pzt::core::ImageRef> result;
  if (criterion == ConsoleFilterCriterion::Reject || criterion == ConsoleFilterCriterion::Dup) {
    std::optional<pzt::core::TagId> tag_id =
        criterion == ConsoleFilterCriterion::Reject
            ? std::optional(reject_tag_id)
            : pzt::core::find_tag_by_name(project_id, pzt::core::tagging::kDuplicateTagName);
    if (!tag_id) return result;  // 项目还没有"重复"系统标签(没跑过 /dedup)
    std::vector<pzt::core::ImageId> ids;
    ids.reserve(base.size());
    for (const auto& r : base) ids.push_back(r.id);
    auto matched = pzt::core::images_with_tag(ids, *tag_id);
    for (const auto& r : base) {
      if (matched.count(r.id)) result.push_back(r);
    }
    return result;
  }
  for (const auto& r : base) {
    auto info = pzt::core::get_image(r.id);
    if (!info) continue;
    bool match = criterion == ConsoleFilterCriterion::Unevaluated
                     ? !info->evaluation.has_value()
                     : (info->evaluation.has_value() && !pzt::core::is_usable(*info->evaluation));
    if (match) result.push_back(r);
  }
  return result;
}

// `:` 输入以 `/` 开头时的命令分发。`ai_eval` 一条命令兼顾三种用法——
// 第一个 token 是范围标记(`*` 或 `#标签名`)时走批量提交；不是的话，说
// 明用户没写范围，整段剩余文本都当成对**当前图片**的额外指引，直接提
// 交单图评估(原来 handle_ai_prompt_flow 里那条路径搬到这里)。用范围标
// 记来判断走哪条路径，而不是猜测第一个词是不是标签名——这正是要求整个
// 控制台必须以 `/` 开头的同一个理由:显式标记，不猜。
ConsoleCommandResult handle_ai_console_command(pzt::core::EvaluationWorker& evaluation_worker,
                                                pzt::core::ProjectId project_id,
                                                pzt::core::ImageId current_image_id,
                                                const std::string& input, int banner_row, int start_col,
                                                int content_cols) {
  auto [command, rest] = split_console_command(input);
  if (command == "help") {
    if (rest.empty()) {
      return ConsoleCommandResult{pzt::cli::i18n::msg_help_overview()};
    }
    auto detail = pzt::cli::i18n::msg_help_command(rest);
    if (!detail) {
      return ConsoleCommandResult{pzt::cli::i18n::err_help_unknown_command(rest)};
    }
    return ConsoleCommandResult{*detail};
  }
  if (command == "dedup") {
    return ConsoleCommandResult{handle_dedup_command(project_id, rest, banner_row, start_col, content_cols)};
  }
  if (command == "tasks") {
    return ConsoleCommandResult{handle_tasks_command(evaluation_worker)};
  }
  if (command == "filter") {
    // 只负责解析,不碰数据库/不算筛选结果——真正的计算放在 cmd_open 里
    // 执行,见 apply_console_filter 的说明。
    if (rest == "clear") {
      return ConsoleCommandResult{"", ConsoleCommandResult::FilterAction::Clear};
    }
    std::optional<ConsoleFilterCriterion> criterion;
    if (rest == "unevaluated") {
      criterion = ConsoleFilterCriterion::Unevaluated;
    } else if (rest == "fail") {
      criterion = ConsoleFilterCriterion::Fail;
    } else if (rest == "reject") {
      criterion = ConsoleFilterCriterion::Reject;
    } else if (rest == "dup") {
      criterion = ConsoleFilterCriterion::Dup;
    }
    if (!criterion) {
      return ConsoleCommandResult{pzt::cli::i18n::err_console_invalid_filter_criterion()};
    }
    return ConsoleCommandResult{"", ConsoleCommandResult::FilterAction::Apply, *criterion};
  }
  if (command == "ai_eval") {
    auto [first_token, extra_guidance] = take_scope_token(rest);
    bool is_batch_scope = first_token == "*" || (!first_token.empty() && first_token[0] == '#');
    if (is_batch_scope) {
      return ConsoleCommandResult{
          handle_ai_eval_command(evaluation_worker, project_id, first_token, extra_guidance)};
    }
    // 没有范围标记:整段 rest 就是对当前图片的额外指引,不需要再拆——供
    // 应商见 resolve_ai_provider()(F-10:读 config.json 的 ai_provider，
    // 默认 Local)。交互式切换 UI 本来就是 docs/M3_PRD.md 明确留到以后
    // 的开放问题,这次不做。
    bool accepted = evaluation_worker.request(current_image_id, resolve_ai_provider(), rest,
                                               pzt::core::load_settings().auto_ai_reject,
                                               resolve_assessment_language(),
                                               resolve_local_model_config());
    if (!accepted) {
      return ConsoleCommandResult{pzt::cli::i18n::msg_ai_processing_pending()};  // 走 status_override,等按键确认
    }
    // 提交成功只是个轻量的确认,不需要用户额外按键才能回到浏览——结果本
    // 身是异步落地、靠 poll 逻辑自动重绘的,这条提示只是"确实提交了"，跟
    // x 键"闪一下"反馈同一个思路,只是这里是完整的一句话,停留久一点方
    // 便看清,然后直接回到顶层空闲状态,不占用一次额外按键。返回空字符
    // 串,外层不会进入"按任意键继续"那个分支。
    move_cursor(banner_row, start_col + 1);
    write_stdout(pad_to(pzt::cli::i18n::msg_ai_processing_submitted(), content_cols));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));  // F-36：usleep 已弃用,统一用 sleep_for
    return ConsoleCommandResult{};
  }
  return ConsoleCommandResult{pzt::cli::i18n::msg_ai_unknown_command(command)};
}

// 顶层 `:` 键:vim 风格的控制台输入口,提交给 handle_ai_console_command
// 分发。控制台现在要求所有输入必须以 `/` 开头——不再有"裸文本=对当前
// 图片的额外指引"这条隐藏路径，这是这次改动明确要解决的问题:用户忘了
// 打 `/`（哪怕只是直接按了回车）不会再被无声当成提交了一次对当前图片
// 的评估请求。空输入、非空但不以 `/` 开头，统一提示"必须以 / 开头"；
// Esc 依然是唯一真正的取消。
ConsoleCommandResult handle_ai_prompt_flow(pzt::core::EvaluationWorker& evaluation_worker,
                                            pzt::core::ProjectId project_id,
                                            pzt::core::ImageId image_id, int banner_row, int start_col,
                                            int content_cols) {
  auto input = read_text_line_with_placeholder(pzt::cli::i18n::msg_ai_prompt_placeholder(),
                                                 banner_row, start_col, content_cols);
  if (!input) return ConsoleCommandResult{};  // Esc,静默取消
  if (input->empty() || (*input)[0] != '/') {
    return ConsoleCommandResult{pzt::cli::i18n::msg_console_requires_slash()};
  }
  return handle_ai_console_command(evaluation_worker, project_id, image_id, *input, banner_row,
                                    start_col, content_cols);
}

// space/x/g/r/e 这几个键要阻塞读一整套 banner 交互(prompt_and_read_key/
// read_text_line 内部自己的循环),不会回到外层主循环顶部,所以右侧菜单栏
// 那一帧画出来之后,整个子菜单流程期间都不会重画。这里在真正调用对应的
// handle_* 之前,对已经画好的那一帧做一次局部覆写:按 key 在 menu_lines
// 里找到那一行,加粗重画。子菜单流程结束、回到外层循环顶部之后,下一帧
// 的整屏重绘会自然画回非加粗状态,不需要额外的"取消加粗"逻辑。
void highlight_active_menu_key(char key, const std::vector<pzt::cli::i18n::MenuLine>& lines,
                                int menu_top_row, int menu_rows, int info_col, int info_cols) {
  for (std::size_t i = 0; i < lines.size() && static_cast<int>(i) < menu_rows; ++i) {
    if (lines[i].key != key) continue;
    move_cursor(menu_top_row + static_cast<int>(i), info_col);
    write_stdout("\x1b[1m" + pad_to(lines[i].text, info_cols) + "\x1b[0m");
    return;
  }
}

}  // namespace

// increment 6.4.2:三面板固定布局(图片区左上约 80% 宽、信息栏右上、
// banner 底部全宽),备用屏幕缓冲区 + 每帧清除上一帧 placement,修复
// 6.4.1 真机测试时发现的图片重叠残留问题。
int cmd_open(const std::vector<std::string>& args) {
  bool debug_mode = false;
  std::vector<std::string> positional;
  for (const auto& a : args) {
    if (a == "--debug") {
      debug_mode = true;
    } else {
      positional.push_back(a);
    }
  }

  std::optional<pzt::core::ProjectId> id =
      positional.empty()
          ? pzt::core::find_project_by_root_path(std::filesystem::current_path().string())
          : pzt::core::find_project_by_name(positional[0]);
  if (!id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_project_not_found().c_str());
    return 1;
  }

  auto opened = pzt::core::open_project(*id);
  if (!opened.ok()) {
    // id 来自刚成功的查找,理论上不该走到这里,但还是按"不假设它不会发生"
    // 的原则处理,而不是直接解引用。
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_project_not_found().c_str());
    return 1;
  }
  const auto& project = opened.value();

  auto images = pzt::core::list_images(*id);
  if (images.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_project_no_images(project.name).c_str());
    return 1;
  }
  // F-09：g 层筛选结果的影子副本——`images` 本身继续驱动导航/渲染/
  // prefetch 不变,`g_filtered_images` 只在 g 切换筛选时同步更新,供
  // `/filter` 在它之上再筛一层、`/filter clear` 时还原用,见下面 `g`
  // 键处理和 `:` 键处理的说明。
  auto g_filtered_images = images;

  // increment 6.4.5:废片系统标签正常应该在 pzt new 时就建好了,这里不是
  // 为了处理迁移——只是同一个幂等、廉价的 find-or-create,顺带兜住"项目
  // 不是通过更新后的 pzt new 建的"这种边界情况,避免后面用这个 id 时崩溃。
  pzt::core::TagId reject_tag_id = pzt::core::ensure_reject_tag(*id);

  // F-12：一次会话读一次就够——界面宽度比例、预取窗口这两个值一旦这个
  // 函数开始跑起来(边框已经按某个比例画出来、PrefetchCache 已经用某个
  // 窗口大小构造完)就没法中途换,不像 resolve_ai_provider() 那样每次调
  // 用都现读也没关系。
  pzt::core::Settings settings = pzt::core::load_settings();

  auto mode = pzt::cli::kitty::detect_terminal_mode();
  if (mode.inside_tmux && !mode.passthrough_ok) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_tmux_passthrough().c_str());
    return 1;
  }

  const int kDebugRows = 8;
  std::size_t frame = 0;
  const int kImageId = 1;
  // 平时(空闲提示/单行状态提示)只用第一行(banner_row);space/g/r 这三
  // 个顶层选项多、容易一行放不下的二级菜单,拆成两行——第一行放带编号的
  // 选项,第二行放字母/Esc 这些固定操作,见 tag_menu.cpp/filter_menu.cpp/
  // recipe_menu.cpp 里对应的 prompt_and_read_key_2line 调用。第二行不用
  // 的时候留空,不常驻显示任何东西。
  const int kBannerRows = 2;
  // 顶层按键菜单挪到右侧面板下半 block,一行一条,取代原来挤在底部 banner
  // 里的一整行文案。q 退出单独留在 banner 那一行常驻显示,不占右侧菜单的
  // 位置,也避免空闲时底部整行空着。
  std::vector<pzt::cli::i18n::MenuLine> menu_lines = pzt::cli::i18n::menu_lines();
  // j/k 转一整圈都没找到未打标签的图片时,不静默无反应——banner 这一帧显示
  // 这条提示而不是空闲内容,显示完就清空,下一次不管按什么键都恢复正
  // 常提示。跟 current_id 一样是这个函数作用域内的纯局部状态,不需要额外
  // 的状态机或定时器。
  std::string status_override;

  // increment 6.4.7:退出时打一行 key-to-render 汇总(count/avg/p95/max)
  // ——PRD 验收标准要求"简单的延迟日志"验证浏览大量图片全程无可感知卡
  // 顿,盯着 debug 面板只保留最后 8 行的实时小窗口没法回头核对整个会话,
  // 需要一份事后能看的汇总,不是新的 core 层能力,纯粹是这个函数自己按
  // 键处理耗时的统计,声明在下面这个块外面,这样块结束(AltScreen/
  // CbreakMode 析构、stderr 换回真实终端)之后还能在这打印。退出时打印一
  // 次,不挂在 --debug 后面。
  std::size_t latency_count = 0;
  double latency_sum_ms = 0.0;
  double latency_max_ms = 0.0;
  std::vector<double> latency_samples;

  {
    // 默认把 stderr(core::PrefetchCache 等的延迟日志)整个丢掉,不跟图片画
    // 到同一块屏幕上;--debug 时改成后台收集,画到屏幕底部专门的 debug 区
    // 域。声明在 prefetch 之前、比它晚析构,这样 prefetch 关闭时可能打的最
    // 后几行日志也能被收住。跟 prefetch 一起放进这个块(而不是 cmd_open 的
    // 外层作用域)是 6.4.7 修的一个问题:这两个如果活到 cmd_open 整个函数
    // 返回才析构,块结束之后想打印的退出汇总/退出提示这些"应该在真实终端
    // 上可见"的输出,实际上还是会被 debug_log 占着的重定向吞掉(不管
    // --debug 开没开,DebugLogRedirect 的析构函数才是真正把 stderr 换回真
    // 实终端的地方)——缩小到这个块的作用域,块结束时 debug_log 先析构、
    // stderr 先换回来,后面的打印才真的能看见。
    // 存的是原始日志条目(一次 fprintf 一条),不是画到屏幕上的行——现在每
    // 条会按显示宽度换行,一条可能占好几个屏幕行,存的原始条目数得比
    // kDebugRows 多几倍,不然换行一展开,面板一开始就没几条真实历史可
    // 看。倍数是拍脑袋定的,不是量出来的精确值,够用就行。
    pzt::cli::term::DebugLogRedirect debug_log(debug_mode,
                                                static_cast<std::size_t>(kDebugRows) * 4);

    // window 默认值 3——PRD 里"合理默认值待真实素材测出"这个待办不受这次
    // 影响,调优留给以后有真实使用数据再说;F-12 之后这个值可以在
    // config.json 里覆盖(prefetch_window),不用改代码重新编译。
    pzt::core::PrefetchCache prefetch(project.root_path, settings.prefetch_window,
                                       pzt::core::decode_preview_file);
    // F-24 会话续点：这个项目上次浏览到的那张图仍在列表里就从它起步,否则
    // (从没浏览过、或那张图已被删/prune 掉)静默落在第一张。
    pzt::core::ImageId current_id = images.front().id;
    if (project.last_image_id) {
      for (const auto& ref : images) {
        if (ref.id == *project.last_image_id) {
          current_id = *project.last_image_id;
          break;
        }
      }
    }
    prefetch.set_current(images, current_id);

    // M3：选片辅助评估,`:` 键触发,见 handle_ai_prompt_flow。生命周期跟
    // prefetch 一样声明在这个块里——退出时析构会等还在跑的请求完成(这是
    // jthread 正确管理生命周期的直接代价,接受这个行为,不做 detach 之类
    // 放弃生命周期管理的取巧方案)。默认参数(真实数据库路径 + 真实
    // request_evaluation),不需要额外传参。
    pzt::core::EvaluationWorker evaluation_worker;
    std::uint64_t ai_last_seen_generation = 0;

    // AltScreen 在 CbreakMode 前构造、后析构:退出时先把输入模式还原、再离
    // 开备用缓冲区,这样即便中途出异常,用户的主屏幕内容也不会被半途切走
    // 又切不回来。
    pzt::cli::term::AltScreen alt_screen;
    pzt::cli::term::CbreakMode cbreak;

    // 上一帧实际渲染的是哪张图——打标签这类不改 current_id 的操作不需要
    // 重新拉取/传输图片本身,只有 current_id 真的变了才需要。
    std::optional<pzt::core::ImageId> last_rendered_id;

    // M1 increment 5:`r v` 临时切换当前图片是否展示风格化效果,纯查看层
    // 面的状态,不碰数据库。导航到新图片时重置为 false(默认展示风格化效
    // 果),只有 style_toggled 为真时才需要在 current_id 没变的情况下也
    // 强制重新走一遍渲染(正常情况下 current_id 不变就不需要重画)。
    bool show_original = false;
    bool style_toggled = false;

    // 上一帧是不是刚显示过 status_override 这种一次性提示——刚显示过的
    // 话,下一次读键不管读到什么都只用来"消除提示",不当成 h/l/j/k/space
    // 的具体动作处理,呼应提示文案里"按任意键继续"这句话:既然说了任意
    // 键,就不应该因为按的不是那几个认识的键就什么反应都没有,也不应该让
    // 这一次按键同时"消除提示"又"顺便导航/打开菜单",那样反而让人搞不清
    // 这次按键到底生效了没有。
    bool showing_status = false;

    // 上一轮是不是 --debug 模式下 poll 超时(没有真实按键)触发的重画——是
    // 的话,这一轮渲染完不打 key-to-render 延迟日志:这条日志的本意是"从
    // 按键到渲染完成"的延迟,超时触发的重画根本没有对应的按键,量出来的
    // 只是这一帧本身的渲染耗时(而且图片这步大概率被跳过,数字会很小),
    // 跟这条日志真正想回答的问题("切图快不快")没关系,混在一起只会让
    // debug 面板看起来像是在不停后台重复干活。
    bool suppress_latency_log = false;

    // increment 6.4.6:当前是否在 g + 数字切出来的筛选视图里,以及筛选到
    // 了哪个标签——跟 current_id 一样是这个函数作用域内的纯局部状态。
    std::optional<pzt::core::TagId> active_filter_tag_id;
    std::string active_filter_tag_name;
    // F-09：控制台二级筛选是否生效,以及是哪个条件——切 g 筛选(应用或
    // 清除)会自动清空这个状态,见 `g` 键处理的说明。
    std::optional<ConsoleFilterCriterion> active_console_filter;

    // F-20：上一帧渲染所用的终端尺寸(cell 行列 + 单 cell 像素)。终端 resize
    // 后布局虽然每轮按最新尺寸重算,但没有整屏清除、也没有强制重画图片,旧
    // 边框会留在原位成残影;用它跟当前尺寸比对,变化时整屏清除 + 强制重画。
    // 初值 0 让首帧就当"尺寸已变"处理(首帧本就要全画,无害)。
    int last_cols = 0, last_rows = 0, last_cell_px_w = 0, last_cell_px_h = 0;

    while (true) {
      auto key_time = std::chrono::steady_clock::now();

      std::size_t index = 0;
      const pzt::core::ImageRef* current_ref = nullptr;
      for (std::size_t i = 0; i < images.size(); ++i) {
        if (images[i].id == current_id) {
          index = i;
          current_ref = &images[i];
          break;
        }
      }

      // 导航检测和 show_original 的重置要放在信息栏绘制之前(这一帧剩下
      // 的部分,包括信息栏和实际渲染,都要看到重置之后的值)——之前这个
      // reset 是在图片渲染那一段(信息栏之后)才做的,导致切到新图片的第
      // 一帧信息栏还在用上一张图片遗留的 show_original,画出"没加粗/没
      // 星号",要等下一帧才更新成正确的加粗状态,真机测试能明显看到这个
      // 卡顿。
      bool navigated = (last_rendered_id != current_id);
      if (navigated) {
        show_original = false;  // 每次导航到新图片,默认展示风格化效果
      }

      auto term_size = pzt::cli::term::get_terminal_size();
      // 拿不到真实尺寸(非 tty、或者终端没上报像素尺寸)时给一组保守的兜
      // 底值,不让布局计算除零或者算出负数区域。
      int total_cols = term_size.valid ? term_size.cols : 80;
      int total_rows = term_size.valid ? term_size.rows : 24;
      int cell_px_w = term_size.valid ? std::max(1, term_size.pixel_width / term_size.cols) : 8;
      int cell_px_h = term_size.valid ? std::max(1, term_size.pixel_height / term_size.rows) : 16;

      // F-20：终端尺寸相对上一帧变了吗?变了就整屏清除(擦掉上一尺寸残留的
      // 边框/文字),并在下面强制重画图片(current_id 没变、navigated 为假,
      // 光靠导航检测触发不了图片重传)。last_* 记录的是"当前帧渲染所用的尺
      // 寸",也是输入循环里 poll 超时判断"要不要重画"的基准。
      bool size_changed = (total_cols != last_cols || total_rows != last_rows ||
                           cell_px_w != last_cell_px_w || cell_px_h != last_cell_px_h);
      last_cols = total_cols;
      last_rows = total_rows;
      last_cell_px_w = cell_px_w;
      last_cell_px_h = cell_px_h;
      if (size_changed) write_stdout("\x1b[2J");

      // 界面默认只占终端宽度的 70%、居中显示,不铺满整个窗口——F-12 之后
      // 这个比例可以在 config.json 里用 ui_width_ratio 覆盖。
      int ui_cols = std::max(20, static_cast<int>(total_cols * settings.ui_width_ratio));
      int start_col = std::max(1, (total_cols - ui_cols) / 2 + 1);

      int content_cols = std::max(1, ui_cols - 2);  // 减去左右各一列边框
      int image_cols = std::max(1, static_cast<int>(content_cols * 0.8));
      int mid_offset = 1 + image_cols;  // 中间竖线相对 start_col 的偏移
      int info_cols = std::max(1, content_cols - image_cols - 2);  // -1: 中间竖线,-1: 留一列空隙
      int info_col = start_col + mid_offset + 2;  // 信息栏内容起始列,跳过竖线和一列空隙

      int border_rows = 2;  // 顶部 + 底部
      int divider_rows = 1 + (debug_mode ? 1 : 0);
      int fixed_rows = border_rows + divider_rows + kBannerRows + (debug_mode ? kDebugRows : 0);
      int top_rows = std::max(1, total_rows - fixed_rows);

      int image_top_row = 2;  // 顶部边框占第 1 行,图片/信息内容从第 2 行开始

      // 右侧面板纵向分成两个 block:上半 metadata、下半菜单(顶层按键提
      // 示,一行一条),中间一条横线分隔——左右宽度比例(图片:信息栏)不
      // 变。菜单 block 内容固定(menu_lines 长度不随图片状态变化),只需要
      // 刚好够显示这些行的高度;metadata 这边标签/风格/AI 点评这些内容经
      // 常需要更多行(点评还会按宽度换行),五五分会让 metadata 在标签多、
      // 点评长的时候被截断,而菜单 block 却总有一截空着没用——改成菜单
      // block 只拿它需要的行数,剩下的全部给 metadata。
      int menu_divider_rows = 1;
      int menu_content_rows = static_cast<int>(menu_lines.size());
      int menu_rows = std::max(1, std::min(top_rows - menu_divider_rows, menu_content_rows));
      int meta_rows = std::max(1, top_rows - menu_divider_rows - menu_rows);
      int meta_bottom_row = image_top_row + meta_rows;  // 不含,metadata 内容到这一行(不含)为止
      int menu_divider_row = meta_bottom_row;
      int menu_top_row = menu_divider_row + 1;

      // 画边框:单个外框 + 图片/信息栏之间的竖线分隔 + 信息栏内部
      // metadata/菜单分隔,风格照抄设计阶段讨论过的 ASCII 示意图,不是四个
      // 各自独立的小方框。
      {
        int row = 1;
        draw_hline(row++, start_col, ui_cols, "┌", "┐", mid_offset, "┬");
        for (int i = 0; i < top_rows; ++i) draw_vlines(row + i, start_col, ui_cols, mid_offset);
        // 只在信息栏那一侧画一条局部横线(从中间竖线到右边框),图片那一侧
        // 这一行还是图片显示区域的一部分,不画线。左端跟中间竖线的交汇处
        // 用"├"(竖线上下都还在延伸,只往右边分支),不是"┼"或"┬"。
        draw_hline(menu_divider_row, start_col + mid_offset, ui_cols - mid_offset, "├", "┤");
        row += top_rows;
        draw_hline(row++, start_col, ui_cols, "├", "┤", mid_offset, "┴");
        if (debug_mode) {
          for (int i = 0; i < kDebugRows; ++i) draw_vlines(row + i, start_col, ui_cols);
          row += kDebugRows;
          draw_hline(row++, start_col, ui_cols, "├", "┤");
        }
        for (int i = 0; i < kBannerRows; ++i) draw_vlines(row + i, start_col, ui_cols);
        row += kBannerRows;
        draw_hline(row, start_col, ui_cols, "└", "┘");
      }
      int debug_top_row = 2 + top_rows + 1;  // 图片区 + 分隔线之后
      int banner_row = debug_top_row + (debug_mode ? kDebugRows + 1 : 0);

      // 信息栏上半 block(metadata):编号、文件名、标签、文件大小。内容行
      // 数随标签数量变化(标签越多占的行越多)——真机测试发现,标签数变少
      // 之后,上一帧比较靠下的内容(比如"大小:"那一行)不会被这一帧覆盖
      // 到,会一直重影在那。先把这个 block 自己的行清空,再画这一帧实际
      // 用到的内容,不管行数怎么变都不会留下上一帧的残留。清空范围只到
      // meta_bottom_row(不含)为止,不能沿用以前"清整个 top_rows"的写
      // 法——那样会把 meta_bottom_row 那一行的分隔线(边框绘制那段代码画
      // 的,横线中间落在 info_col 这段范围内的部分)每帧又拿空格盖掉,只剩
      // 左右两端的"├""┤"看得见,是真机反馈的那个"分割线显示不全"的成因。
      // 下半 block(菜单)内容固定不随图片变化,每帧原样整行覆盖写就是自
      // 己的清空,不需要额外清空这一段。
      {
        // M3 之前这里是先整块清空(每一行一次 move_cursor+write_stdout)、
        // 再逐行画内容(又是一轮各自独立的 move_cursor+write_stdout)——
        // metadata block 拉高之后(这块之前只有 top_rows 的一半,现在占
        // 了绝大部分)两轮加起来一帧要发出去几十次独立的写系统调用,真机
        // 反馈能看出明显的"先闪一片空白、再画出内容"的闪烁。改成两轮都
        // 拼进同一个字符串、最后一次性 write_stdout——闪烁的成因是"两轮
        // 独立的系统调用之间终端有机会先渲染出中间那个空白状态",不是
        // "分两轮画"这件事本身,拼进同一次系统调用发出去,终端不会有机
        // 会停在中间态。
        //
        // 这两轮缺一不可:emit_line/emit_style_line 只在"这一行本帧确实
        // 有内容"时才写(节省无意义的字节拼接)，"row++; // 空一行"这种
        // 段落之间的分隔行本帧完全不会被写到——如果只清"最后一行内容之
        // 后"的尾部(之前一版这么写过，是个 bug)，标签数、点评长度这些
        // 会改变行位置的内容一旦跨帧变化，之前帧遗留在这些"空一行"位置
        // 上的字符永远没人覆盖，会一直显示着（真机反馈过"看到两个
        // score"，就是这么来的——上一次点评占的行数跟这一次不一样，AI
        // Score 那一行的残影跟这一帧新画的重叠在了一起）。这里老老实实
        // 把 [image_top_row, meta_bottom_row) 整个范围都清一遍,再把内容
        // 紧接着写进同一个缓冲区(处理顺序上晚于清空,同一个位置以后写
        // 的生效),不留任何一行是"这两轮都没碰过"的。
        std::string out;
        for (int r = image_top_row; r < meta_bottom_row; ++r) {
          out += "\x1b[" + std::to_string(r) + ";" + std::to_string(info_col) + "H";
          out += pad_to("", info_cols);
        }
        int row = image_top_row;
        // metadata 现在只有信息栏上半 block 的高度可用(meta_bottom_row 之
        // 前)——标签多、风格层级深的时候可能装不下,超出的部分直接不画,
        // 不做省略号提示或者滚动,这属于"具体分布下一步再优化"的范围,这
        // 一步先保证不会画穿到下半的菜单 block 里。
        auto emit_line = [&](const std::string& text, bool bold = false) {
          if (row < meta_bottom_row) {
            out += "\x1b[" + std::to_string(row) + ";" + std::to_string(info_col) + "H";
            std::string padded = pad_to(text, info_cols);
            out += bold ? "\x1b[1m" + padded + "\x1b[0m" : padded;
          }
          ++row;
        };

        // increment 6.4.6:筛选状态拼在这一行后面,不新增一行——这样下面
        // 每一行(文件名、标签、大小)不管是不是在筛选视图里都是完全一样
        // 的行号计算,切换筛选状态时不会有内容跳动。
        std::string index_line =
            "[" + std::to_string(index + 1) + "/" + std::to_string(images.size()) + "]";
        // 反馈:标签前缀太长容易被截断,改成 "TagName | criterion" 这种
        // 紧凑写法,两层筛选各自独立、可以同时出现。
        index_line += pzt::cli::i18n::info_active_filters_label(
            active_filter_tag_id ? std::optional<std::string>(active_filter_tag_name) : std::nullopt,
            active_console_filter
                ? std::optional<std::string>(console_filter_criterion_keyword(*active_console_filter))
                : std::nullopt);
        emit_line(index_line);

        emit_line(current_ref ? current_ref->file_name : "?");

        row++;  // 空一行
        auto tags = current_ref ? pzt::core::tags_for_image(current_ref->id)
                                 : std::vector<pzt::core::TagSummary>{};
        std::vector<std::string> tag_line_tokens = {pzt::cli::i18n::info_tags_label()};
        if (tags.empty()) {
          tag_line_tokens.push_back(pzt::cli::i18n::info_none_label());
        } else {
          for (const auto& t : tags) tag_line_tokens.push_back(tag_token(t));
        }
        for (const auto& line : wrap_tokens(tag_line_tokens, static_cast<std::size_t>(info_cols))) {
          emit_line(line);
        }

        row++;  // 空一行
        auto info = current_ref ? pzt::core::get_image(current_ref->id) : std::nullopt;
        if (info) {
          emit_line(pzt::cli::i18n::info_size_label(format_size(info->file_size)));
          emit_line(pzt::cli::i18n::info_source_label(info->kind == "raw"));
          // 拍摄时间这一行经常超出信息栏窄列的宽度被截断,改成跟"风格:"一
          // 样的标题行 + 缩进值行,见 i18n.cpp 里 info_captured_at_heading/
          // format_captured_at 的说明。
          emit_line(pzt::cli::i18n::info_captured_at_heading());
          emit_line("  " + pzt::cli::i18n::format_captured_at(info->captured_at));
        }

        // M1 increment 3:在真正的 `r` 交互(increment 6)和预览渲染
        // (increment 5)落地之前,先在信息栏露出"这张图应用了哪个风格",
        // 方便用 apply-debug 之类的调试命令验证时能直观看到结果,不用每
        // 次都手动查数据库。两层模型(预设/version)用两级缩进画成一棵小
        // 树,不是拼成一行文本——真机测试发现拼一行会在信息栏这种窄列里
        // 被截断,例如"风格: Standard: MyStandard"就被切成了"风格:
        // Standard: MyStanda",看不全。
        row++;  // 空一行
        // M3 修订：Recipe 标签跟值(预设名/(无))合并一行，不再各占一行——
        // 给下面的选片评估腾地方。只有真的选了一个具体保存的 version(不
        // 是直接用预设本身，见 style->version_name 是不是有值)才多占一
        // 行——用预设默认状态是最常见的情况，这种情况下没有多余信息要
        // 展示，不该白占一行。
        auto recipe_id = current_ref ? pzt::core::get_image_recipe(current_ref->id) : std::nullopt;
        auto style = recipe_id ? pzt::core::describe_recipe(*recipe_id) : std::nullopt;
        // M1 increment 5:当前实际渲染的是风格化效果时标出来(`r v` 切到
        // 原图预览时取消),直接呼应"现在看到的是不是风格化效果"这个状
        // 态。真机测试发现单靠 ANSI 粗体(`\x1b[1m`)不可靠——很多终端的
        // 中文字体没有配置独立的粗体字重,ASCII 文本(比如预设名"Origin")
        // 会正常加粗,但中文 version 名字(比如"亮一点")的字重不会变,不
        // 是代码逻辑的问题,是终端/字体限制。改用不依赖字重的文字标记
        // (`*`)当主要信号,粗体转义码还留着(在支持的终端上锦上添花),
        // 但不再是唯一的指示方式。星号不显示时换成等宽的空格而不是整个
        // 去掉——真机反馈过直接去掉会导致名字的列位置随着 `r v` 切换来
        // 回跳动，看着很别扭。标签本身("风格:"/"Recipe:")不加粗，只加
        // 粗星号+名字这一段——两段分开各自 pad_to 再拼起来，不是对整行
        // 结果做字符串切片插入转义码(那样要精确计算标签的字节长度，容
        // 易因为中英文标签宽度不同出 bug)。
        bool style_active = style.has_value() && !show_original;
        {
          std::string label = pzt::cli::i18n::info_style_label() + " ";
          std::size_t label_width = display_width(label);
          std::size_t value_width =
              static_cast<std::size_t>(info_cols) > label_width
                  ? static_cast<std::size_t>(info_cols) - label_width
                  : 0;
          std::string value_text;
          if (!style) {
            value_text = pzt::cli::i18n::info_none_label();
          } else {
            value_text = (style_active ? "* " : "  ") + style->preset_name;
          }
          if (row < meta_bottom_row) {
            std::string padded_value = pad_to(value_text, value_width);
            out += "\x1b[" + std::to_string(row) + ";" + std::to_string(info_col) + "H";
            out += label;
            out += style_active ? "\x1b[1m" + padded_value + "\x1b[0m" : padded_value;
          }
          ++row;
        }
        if (style && style->version_name) {
          if (row < meta_bottom_row) {
            std::string marker = style_active ? "  * " : "    ";
            std::string padded = pad_to(marker + *style->version_name, info_cols);
            out += "\x1b[" + std::to_string(row) + ";" + std::to_string(info_col) + "H";
            out += style_active ? "\x1b[1m" + padded + "\x1b[0m" : padded;
          }
          ++row;
        }

        // W2026-07-21：`:` 触发的 AI 点评——标题行"AI 点评"+一段模型给的文
        // 字 assessment。可用时不显示可用性(避免"选片/Culling"跟 agent 功能
        // 混淆)，只有 unusable 时在 assessment 前加粗显示一行"不可用"。复用上
        // 面查过的 info。assessment 长度不可控——按显示宽度硬换行,跟标签/风格
        // 一样受 meta_bottom_row 的越界裁剪保护,装不下的部分直接不画。
        row++;  // 空一行
        if (!info || !info->evaluation) {
          emit_line(pzt::cli::i18n::evaluation_none_label());
        } else {
          const auto& eval = *info->evaluation;
          emit_line(pzt::cli::i18n::evaluation_comment_label());
          if (eval.unusable) {
            emit_line(pzt::cli::i18n::evaluation_unusable_label(), /*bold=*/true);
          }
          for (const auto& line : wrap_text(eval.assessment, static_cast<std::size_t>(info_cols))) {
            emit_line(line);
          }
        }

        write_stdout(out);
      }

      // 右侧面板下半 block:顶层按键菜单,一行一条,静态内容(不依赖当前
      // 图片状态),超出可用行数的部分直接不画。
      {
        for (std::size_t i = 0; i < menu_lines.size() && static_cast<int>(i) < menu_rows; ++i) {
          move_cursor(menu_top_row + static_cast<int>(i), info_col);
          write_stdout(pad_to(menu_lines[i].text, info_cols));
        }
      }

      // --debug 时,图片/信息栏下方专门留出来的滚动 debug 区——按帧重画最
      // 新的 kDebugRows 行,不是真正的终端滚动区域,但对用户来说效果一样:
      // 新日志进来,老的自然被挤出显示范围。每条原始日志先按显示宽度换
      // 行展开成若干屏幕行,再对展开后的结果取最后 kDebugRows 行——这样
      // 一条长日志(比如完整的 AI 请求/响应)会占多行显示,不是硬截断成
      // 一行看不全。
      if (debug_mode) {
        auto lines = debug_log.snapshot();
        std::vector<std::string> display_rows;
        for (const auto& line : lines) {
          auto wrapped = wrap_text(line, static_cast<std::size_t>(content_cols));
          display_rows.insert(display_rows.end(), wrapped.begin(), wrapped.end());
        }
        std::size_t begin =
            display_rows.size() > static_cast<std::size_t>(kDebugRows)
                ? display_rows.size() - static_cast<std::size_t>(kDebugRows)
                : 0;
        for (int i = 0; i < kDebugRows; ++i) {
          move_cursor(debug_top_row + i, start_col + 1);
          std::size_t idx = begin + static_cast<std::size_t>(i);
          write_stdout(pad_to(idx < display_rows.size() ? display_rows[idx] : "", content_cols));
        }
      }

      // Banner:固定在图片/信息栏下方最后两行,边框内全宽。顶层按键提示大
      // 部分挪到右侧菜单 block 了,导航键(h/l、j/k)和 q 退出分两行常驻显
      // 示在这里——避免第二行一直空着不好看。有状态提示、或者 space/g/r
      // 这些次级菜单需要临时输入/确认时,两行的内容临时换成那些(那些调
      // 用点直接往这两行 move_cursor+write_stdout,不需要这次改动)。
      move_cursor(banner_row, start_col + 1);
      showing_status = !status_override.empty();
      if (showing_status) {
        // status_override 里的消息大多自带一个尾随空格(原来是跟banner
        // 默认提示的视觉留白风格一致,现在这条只是延续同样的拼接方式),
        // 直接拼接"  按任意键继续"会在两者之间留出一大段空白,看起来像隔
        // 得很远——先去掉消息自己的尾随空格,用逗号衔接而不是额外的空格。
        // 状态提示本身就是单行,第二行照样清空,不跟着凑一行内容。
        std::string trimmed = status_override;
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        write_stdout(pad_to(pzt::cli::i18n::msg_press_any_key_to_continue(trimmed), content_cols));
        move_cursor(banner_row + 1, start_col + 1);
        write_stdout(pad_to("", content_cols));
      } else {
        write_stdout(pad_to(pzt::cli::i18n::nav_bar_line1(), content_cols));
        move_cursor(banner_row + 1, start_col + 1);
        write_stdout(pad_to(pzt::cli::i18n::nav_bar_line2(), content_cols));
      }
      status_override.clear();  // 只显示这一帧,不管接下来按了什么键都恢复正常提示

      // 图片放在信息栏/banner 之后画:真机测试反馈图片显示出来之后,右边
      // 信息栏和底部 banner 的文字有明显的滞后才跟上,怀疑是 Ghostty 处理
      // Kitty 图片协议命令(读临时文件、解码、合成)这一步在它自己的主循环
      // 里是同步/阻塞的,会顺带卡住紧跟在图片命令后面的文字——即便我们这
      // 边是几乎同时把所有这些控制序列写出去的。这几行文字本身很小、写
      // 出去的成本可以忽略,调整顺序让文字先于图片写出去,这样即便终端处
      // 理图片这一步确实慢,文字至少能立刻显示,不用跟着一起卡住。打标签
      // 这类操作不会改 current_id,不需要重新清除/传输同一张图——真机测试
      // 发现,不加这个判断的话,打个标签也会因为整帧重画而卡顿一下,尽管
      // 图片内容根本没变。只有 current_id 真的变了才重新走一遍"清掉上一
      // 帧的图 -> 取解码结果 -> 缩放 -> 传输"这一整套。`navigated` 在这
      // 一帧最前面(信息栏绘制之前)已经算过、`show_original` 也已经在
      // 那里重置过,这里直接复用,不重新算一遍。
      if (navigated || style_toggled || size_changed) {
        // 每帧先清掉上一帧的图,再画新的——这是修复 6.4.1 重叠残留问题的
        // 关键一步,没有它,旧 placement 不会自动消失。失败(比如
        // WriteFailed)这里不特殊处理——下面马上要写新的 placement 覆盖
        // 同一个 id,没有比"继续往下走"更好的补救动作,显式 (void) 丢弃
        // 而不是让 [[nodiscard]] 警告挂着没人处理(F-19)。
        (void)pzt::cli::kitty::clear_placement(STDOUT_FILENO, mode, kImageId);

        auto decoded = prefetch.get(current_id);
        if (decoded.ok()) {
          // F-14：decoded.value() 是 shared_ptr(指向缓存里那份不可变像素),
          // decoded 在这个块作用域内一直存活、持有引用,解引用得到的 img 引
          // 用在整段渲染期间有效。
          const auto& img = *decoded.value();
          // 让图片在面板里居中、四周留一点空隙,而不是贴着左边框/上边
          // 框——fit_within 只保证"不超出"这个框,不保证"居中",长宽比
          // 跟面板不完全匹配时(几乎总是这样)不作处理的话,多出来的空白
          // 会全部堆在右边/下边,图片贴着另外两条边。先从可用区域里减掉
          // 一份固定 padding 再传给 fit_within,保证贴得最紧的那个维度
          // 也留有空隙;再用算出来的目标尺寸相对完整的 image_cols x
          // top_rows 框计算居中偏移,把剩余的宽松空间平均分到两侧。
          const int kImagePaddingCols = 2;  // 终端 cell 不是正方形,横向
          const int kImagePaddingRows = 1;  // 留白数值上比纵向大一点,视觉才均衡
          int avail_cols = std::max(1, image_cols - kImagePaddingCols * 2);
          int avail_rows = std::max(1, top_rows - kImagePaddingRows * 2);
          auto fit = pzt::cli::kitty::fit_within(img.width, img.height, avail_cols * cell_px_w,
                                                  avail_rows * cell_px_h);
          int target_cols = std::max(1, fit.width / cell_px_w);
          int target_rows = std::max(1, fit.height / cell_px_h);
          int offset_cols = (image_cols - target_cols) / 2;
          int offset_rows = (top_rows - target_rows) / 2;

          // 真机测试确认过:每帧把原始分辨率的 RGBA(可能几 MB 到近十 MB)
          // 整个丢给终端,终端自己读临时文件+解码+缩放显示,是切图卡顿的
          // 实际来源——即便我们这边 prefetch 已经命中、解码耗时为 0。先
          // 在这边缩小到面板大致能显示的尺寸,大幅减少终端侧要处理的数
          // 据量。
          auto resized = pzt::core::resize_rgba(img, fit.width, fit.height);
          const auto& downsampled = resized.ok() ? resized.value() : img;

          // M1 increment 5:在降采样之后、发给终端之前应用 recipe。
          // thread_count=1 同步执行——Phase 0 spike 已经验证过预览分辨率
          // 下这一步足够便宜(10-22ms),不需要额外的后台线程或缓存;这个
          // if 块本来就只在导航或 `r v` 切换时才跑,不会每帧都重算。
          // show_original 为真时(用户按了 r v 切到原图预览)跳过渲染。
          std::optional<pzt::core::DecodedImage> styled;
          auto recipe_id = pzt::core::get_image_recipe(current_id);
          if (recipe_id && !show_original) {
            auto render_result = pzt::core::render(downsampled, *recipe_id, 1);
            if (render_result.ok()) styled = std::move(render_result.value());
            // render 失败(比如引用了一个数据损坏的 recipe_id)时静默回退
            // 到未处理的画面,不阻断浏览,跟"图片解码失败,跳过"是同一种
            // 防御精神。
          }
          const auto& to_render = styled ? *styled : downsampled;

          move_cursor(image_top_row + offset_rows, start_col + 1 + offset_cols);
          std::string tmp_path = pzt::cli::kitty::make_tmp_path(
              std::to_string(getpid()) + "_" + std::to_string(frame++));
          auto rendered = pzt::cli::kitty::render_rgba_via_tmpfile(
              STDOUT_FILENO, mode, to_render, kImageId, tmp_path, target_cols, target_rows);
          if (!rendered.ok()) {
            std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_render_failed().c_str());
          }
        } else {
          std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_decode_failed().c_str());
        }
        last_rendered_id = current_id;
        style_toggled = false;
      }

      if (!suppress_latency_log) {
        double key_to_render_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - key_time)
                .count();
        std::fprintf(stderr, "[pzt open] key-to-render %.2fms\n", key_to_render_ms);
        ++latency_count;
        latency_sum_ms += key_to_render_ms;
        latency_max_ms = std::max(latency_max_ms, key_to_render_ms);
        latency_samples.push_back(key_to_render_ms);
      }

      char c = 0;
      if (showing_status) {
        // 刚显示过一次性提示("按任意键继续"),这一次读键不管读到什么字
        // 节都只用来消除提示、跳回外层循环重画一次正常画面,不当成
        // h/l/j/k/space 的具体动作执行——否则这一次按键会同时"消除提示"
        // 又"顺便导航/打开菜单",容易让人搞不清这次按键到底生效了没有。
        ssize_t n = read(STDIN_FILENO, &c, 1);
        showing_status = false;
        suppress_latency_log = false;  // 这一轮读到了真实按键(用来消除提示)
        if (n <= 0) break;  // 真正的 EOF/出错,当退出处理
        continue;
      }

      // 不支持的键直接在这个内层循环里吃掉,继续读下一个字节——不 continue
      // 回外层 while,那样会导致整个画面(边框、图片、信息栏、banner)重新
      // 渲染一遍,一次误按不支持的键就能看到明显的闪烁。始终带超时 poll:
      // --debug 时超时无条件重画刷新 debug 面板;有 AI 请求在跑时超时要先查
      // consume_new_result 有没有真的拿到新结果,没有就当没发生过继续等,不
      // 触发外层重绘("poll 重绘只在真正需要时才发生")。F-20:纯浏览态(不
      // 开 debug、无 AI)以前是纯阻塞 read,resize 没有按键就永远察觉不到;
      // 现在也 poll,超时后只有终端尺寸相对上一帧变了才 break 去重画(整屏清
      // 除 + 图片重传由帧顶 size_changed 处理),尺寸没变就继续等,不无谓刷新。
      bool timed_out = false;
      while (true) {
        bool poll_active = debug_mode || evaluation_worker.has_pending();
        int poll_ms = poll_active ? 300 : 250;  // 纯浏览态 250ms 仅用于察觉 resize
        if (!stdin_ready(poll_ms)) {
          if (poll_active) {
            if (!debug_mode) {
              if (!evaluation_worker.consume_new_result(ai_last_seen_generation)) {
                continue;
              }
              // F-03：确认拿到新结果(不是超时空转)之后,顺带看一眼这次
              // 落地的请求是不是失败的——失败之前只打 stderr,不开
              // --debug 时用户完全看不到,提交之后要么等到结果、要么永
              // 远等不到也不知道为什么。debug 模式下失败已经能从 debug
              // 面板的日志流看到,不在这里重复弹一次提示。
              if (auto failure = evaluation_worker.take_last_failure()) {
                status_override =
                    pzt::cli::i18n::msg_ai_evaluation_failed(failure->image_id, failure->error);
              }
            }
            timed_out = true;
            break;
          }
          // 纯浏览态:只有终端尺寸变了才值得重画,否则继续阻塞式等待。
          auto now = pzt::cli::term::get_terminal_size();
          int now_cols = now.valid ? now.cols : 80;
          int now_rows = now.valid ? now.rows : 24;
          int now_pw = now.valid ? std::max(1, now.pixel_width / now.cols) : 8;
          int now_ph = now.valid ? std::max(1, now.pixel_height / now.rows) : 16;
          if (now_cols != last_cols || now_rows != last_rows || now_pw != last_cell_px_w ||
              now_ph != last_cell_px_h) {
            timed_out = true;  // 借 timed_out 语义:没有按键、只是重画
            break;
          }
          continue;
        }
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
          c = 'q';
          break;
        }
        if (c == 'q' || c == 'h' || c == 'l' || c == 'j' || c == 'k' || c == ' ' || c == 'x' ||
            c == 'f' || c == 'r' || c == 'e' || c == ':') {
          break;
        }
      }
      if (timed_out) {
        suppress_latency_log = true;  // 没有按键,只是刷新画面(debug 面板或者 AI 新结果),不处理导航
        continue;
      }
      suppress_latency_log = false;  // 这一轮确实读到了真实按键
      if (c == 'q') {
        // 反馈:队列里还有评估任务时直接退出会静默丢掉还没开始处理的那
        // 部分(EvaluationWorker 析构只等"已经在处理"的那一个完成，不
        // 会继续消费 queue_ 里剩下的请求)——加一次确认，给用户反悔的
        // 机会，跟 /dedup 那个"还有未评估图片"的两行确认同一个先例。
        auto pending_status = evaluation_worker.queue_status();
        int pending_count =
            static_cast<int>(pending_status.queued) + (pending_status.processing ? 1 : 0);
        if (pending_count > 0) {
          char confirm = prompt_and_read_key_2line(
              pzt::cli::i18n::msg_quit_confirm_pending_line1(pending_count),
              pzt::cli::i18n::msg_quit_confirm_pending_line2(), banner_row, start_col, content_cols);
          if (confirm != 'y' && confirm != 'Y') continue;  // 取消退出,回到主循环
        }
        break;
      }

      if (c == 'h') {
        current_id = pzt::core::prev_image(images, current_id).value_or(current_id);
      } else if (c == 'l') {
        current_id = pzt::core::next_image(images, current_id).value_or(current_id);
      } else if (c == 'j') {
        // 筛选视图里每张图按定义都至少有筛选到的那个标签,"下一个未打标
        // 签的" 在这个语境下没有意义,永远立刻报"全部打完"——不是 bug,但
        // 体验上很尴尬,筛选生效时退化成跟 l 一样的普通下一张。
        if (active_filter_tag_id) {
          current_id = pzt::core::next_image(images, current_id).value_or(current_id);
        } else {
          auto next = pzt::core::next_untagged(images, current_id);
          if (next) {
            current_id = *next;
          } else {
            status_override = pzt::cli::i18n::msg_all_tagged();
          }
        }
      } else if (c == 'k') {
        if (active_filter_tag_id) {
          current_id = pzt::core::prev_image(images, current_id).value_or(current_id);
        } else {
          auto prev = pzt::core::prev_untagged(images, current_id);
          if (prev) {
            current_id = *prev;
          } else {
            status_override = pzt::cli::i18n::msg_all_tagged();
          }
        }
      } else if (c == ' ') {
        if (current_ref) {
          highlight_active_menu_key(' ', menu_lines, menu_top_row, menu_rows, info_col, info_cols);
          // F-01：现查而不是缓存在循环外——"重复"标签可能是本次浏览会
          // 话期间第一次跑 /dedup 才创建的,find_tag_by_name 找不到就是
          // nullopt,不会创建它(打开菜单不该有创建标签的副作用)。
          auto duplicate_tag_id =
              pzt::core::find_tag_by_name(*id, pzt::core::tagging::kDuplicateTagName);
          status_override = handle_space_key(*id, reject_tag_id, duplicate_tag_id,
                                              current_ref->id, banner_row, start_col, content_cols);
        }
        // current_id 不变,跟其它分支一样落到下面的 set_current + 循环顶部
        // 整屏重绘,信息栏会自然显示打标签之后的结果。
      } else if (c == 'x') {
        // 标记为废片的直达快捷键,等价于 space + 0/space - 0,但不用先开
        // 菜单——废片预期是使用频率最高的标签,值得单独开一个键。做成开
        // 关切换(已经标了就摘掉):误按一下能直接再按一次撤销,不需要先
        // 开 space 菜单走摘除流程。
        if (current_ref) {
          // x 是瞬时切换,不像 space/g/r 那样会停在一个交互提示上等用
          // 户——加粗写出去之后如果立刻继续执行、下一帧又整屏重绘恢复正
          // 常,这一下"闪"太快,人眼基本看不出来(真机反馈"按 x 没反应")。
          // 主动停一小段时间,让这次加粗有机会被看见,再继续实际的打标
          // 签/摘标签动作。
          highlight_active_menu_key('x', menu_lines, menu_top_row, menu_rows, info_col, info_cols);
          std::this_thread::sleep_for(std::chrono::milliseconds(150));  // 150ms,肉眼可感知的"闪一下",但不会让人觉得卡顿(F-36：usleep -> sleep_for)

          auto current_tags = pzt::core::tags_for_image(current_ref->id);
          bool already_tagged = std::any_of(
              current_tags.begin(), current_tags.end(),
              [&](const auto& t) { return t.id == reject_tag_id; });
          if (already_tagged) {
            auto result = pzt::core::remove_tag(current_ref->id, reject_tag_id);
            status_override = result.ok() ? "" : pzt::cli::i18n::err_remove_tag_failed();
          } else {
            status_override = handle_add_tag_result(reject_tag_id, current_ref->id, banner_row,
                                                     start_col, content_cols);
          }
        }
      } else if (c == 'f') {
        // 点 3：筛选入口键从 g 改成 f(筛选/Filter 首字母)——f + 数字切换
        // 到只浏览该标签下图片的筛选视图,f + f 清除筛选回到完整项目,数
        // 字编号复用跟 space 菜单同一套 tags_for_menu。
        highlight_active_menu_key('f', menu_lines, menu_top_row, menu_rows, info_col, info_cols);
        auto tags = tags_for_menu(*id);
        // F-01：跟 space 分支同样的现查逻辑,见那边的说明。
        auto duplicate_tag_id =
            pzt::core::find_tag_by_name(*id, pzt::core::tagging::kDuplicateTagName);
        auto decision =
            handle_g_key_prompt(reject_tag_id, duplicate_tag_id, tags, banner_row, start_col, content_cols);

        if (decision.action == GKeyAction::ApplyFilter) {
          // 真机测试反馈 g + 数字筛选有明显卡顿,查出来是 image_tags 按
          // tag_id 过滤没有索引可用(见 core/db/schema.cpp 的说明,已经
          // 补上索引)——这里打一下查询本身的耗时,debug 面板能直接看到
          // 这一步占了多少,跟后面"切到新图片要重新解码"那部分区分开。
          auto filter_t0 = std::chrono::steady_clock::now();
          auto filtered = pzt::core::filter_by_tag(decision.tag_id);
          double filter_query_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - filter_t0)
                                        .count();
          std::fprintf(stderr, "[pzt open] filter_by_tag tag_id=%lld %.2fms\n",
                       static_cast<long long>(decision.tag_id), filter_query_ms);
          if (!filtered.ok()) {
            status_override = pzt::cli::i18n::err_filter_failed();  // 结构上不可能,防御性处理
          } else if (filtered.value().empty()) {
            status_override = pzt::cli::i18n::msg_filter_no_images();  // 拒绝切换,images/current_id 不变
          } else {
            // 注意顺序:先用 filtered.value() 算出 new_current,再 move,
            // 不然 move 之后 filtered.value() 已经是空壳。
            pzt::core::ImageId new_current =
                resolve_current_after_switch(filtered.value(), current_id);
            images = std::move(filtered.value());
            current_id = new_current;
            active_filter_tag_id = decision.tag_id;
            active_filter_tag_name = decision.tag_name;
            // F-09：切到新的 g 筛选,二级筛选跟着自动清空(已跟用户确
            // 认),g_filtered_images 同步成这次的结果,供 /filter 在它
            // 之上再筛。
            g_filtered_images = images;
            active_console_filter.reset();
          }
        } else if (decision.action == GKeyAction::ClearFilter) {
          if (active_filter_tag_id) {
            auto full = pzt::core::list_images(*id);
            pzt::core::ImageId new_current = resolve_current_after_switch(full, current_id);
            images = std::move(full);
            current_id = new_current;
            active_filter_tag_id.reset();
            active_filter_tag_name.clear();
            // F-09：同上,清除 g 筛选也要清空二级筛选、同步 g_filtered_images。
            g_filtered_images = images;
            active_console_filter.reset();
          }
          // 不在筛选中时 f+f 是空操作:不查库、不提示,静默——避免每次误
          // 按 f+f 在未筛选状态下也触发一次不必要的 list_images 查询。
        } else if (decision.action == GKeyAction::Cancel) {
          // decision.status 在 Esc 时是空字符串(静默),按了个不认识的
          // 键时带一句"无效按键"提示——跟 r 键的 handle_r_key 保持一致,
          // 见 handle_g_key_prompt 的说明。
          status_override = decision.status;
        }
      } else if (c == 'r') {
        // increment 6:完整的 `r` 前缀键交互,见 handle_r_key。应用/清除
        // 需要重新走一遍渲染(recipe_id 变了或者切到原图预览),交给
        // style_toggled 触发;创建/删除不影响当前图片的 recipe_id,不需
        // 要强制重画。
        if (current_ref) {
          highlight_active_menu_key('r', menu_lines, menu_top_row, menu_rows, info_col, info_cols);
          auto outcome =
              handle_r_key(current_ref->id, banner_row, start_col, content_cols);
          status_override = outcome.status;
          if (outcome.action == RKeyAction::Applied || outcome.action == RKeyAction::Cleared) {
            show_original = false;
            style_toggled = true;
          } else if (outcome.action == RKeyAction::Toggled) {
            show_original = !show_original;
            style_toggled = true;
          }
        }
      } else if (c == 'e') {
        // 顶层导出快捷键。没有 active filter(g 层标签筛选和控制台二级
        // 筛选都没生效)时保持原样:单键直接导出当前这一张。有筛选生效
        // 时弹一个二级菜单再选"当前照片"还是"当前筛选范围"——点 2：
        // 以前"导出任意标签"挂在 g+e 下面,交互起来很诡异,已经退休；
        // "导出全部"和"导出某个标签组"也刻意不共用同一个快捷键,避免
        // 手滑导出了不想要的范围。
        if (current_ref) {
          highlight_active_menu_key('e', menu_lines, menu_top_row, menu_rows, info_col, info_cols);
          bool filter_active = active_filter_tag_id.has_value() || active_console_filter.has_value();
          if (!filter_active) {
            status_override = handle_export_current_flow(current_ref->id, current_ref->file_name,
                                                           banner_row, start_col, content_cols);
          } else {
            char sub = prompt_and_read_key(pzt::cli::i18n::msg_export_submenu_prompt(), banner_row,
                                            start_col, content_cols);
            if (sub == 'e') {
              status_override = handle_export_current_flow(current_ref->id, current_ref->file_name,
                                                             banner_row, start_col, content_cols);
            } else if (sub == 'f') {
              // 目标本身就是废片/重复时不排除——跟 /ai_eval、/dedup、
              // pzt export 的对称例外规则一致，只是这里"目标"可能来自
              // g 层标签，也可能来自控制台二级筛选 criterion。
              auto duplicate_tag_id =
                  pzt::core::find_tag_by_name(*id, pzt::core::tagging::kDuplicateTagName);
              bool target_is_reject =
                  (active_filter_tag_id && *active_filter_tag_id == reject_tag_id) ||
                  (active_console_filter && *active_console_filter == ConsoleFilterCriterion::Reject);
              bool target_is_dup =
                  (active_filter_tag_id && duplicate_tag_id &&
                   *active_filter_tag_id == *duplicate_tag_id) ||
                  (active_console_filter && *active_console_filter == ConsoleFilterCriterion::Dup);
              status_override = handle_export_filtered_flow(
                  *id, images, target_is_reject || settings.export_reject,
                  target_is_dup || settings.export_dup, banner_row, start_col, content_cols);
            } else if (sub != 0x1B) {
              // 不是 Esc,也不是 e/f 里的任何一个——给一句反馈而不是完全
              // 没反应,跟这个文件里其它子菜单同样的约定。Esc 静默取消。
              status_override = pzt::cli::i18n::recipe_menu_invalid_key();
            }
          }
        }
      } else if (c == ':') {
        // M3:vim 风格的额外指引输入,提交给 EvaluationWorker 异步评估。
        // current_id 不变,跟 space/x/r/e 一样只走 status_override 原地
        // 刷新——结果落地由上面的 poll 逻辑触发重绘,不是这里同步等待。
        // F-09：`/filter` 是例外,它会改浏览池状态,返回类型从纯
        // std::string 升级成 ConsoleCommandResult 之后在这里执行。
        if (current_ref) {
          highlight_active_menu_key(':', menu_lines, menu_top_row, menu_rows, info_col, info_cols);
          auto console_result = handle_ai_prompt_flow(evaluation_worker, *id, current_ref->id,
                                                        banner_row, start_col, content_cols);
          status_override = console_result.status;
          if (console_result.action == ConsoleCommandResult::FilterAction::Clear) {
            // 没有活跃二级筛选时是静默 no-op,跟 f+f 空筛选同一个约定。
            if (active_console_filter) {
              current_id = resolve_current_after_switch(g_filtered_images, current_id);
              images = g_filtered_images;
              active_console_filter.reset();
            }
          } else if (console_result.action == ConsoleCommandResult::FilterAction::Apply) {
            auto filtered =
                apply_console_filter(*id, g_filtered_images, reject_tag_id, console_result.criterion);
            if (filtered.empty()) {
              status_override = pzt::cli::i18n::msg_console_filter_no_images();  // images/current_id 不变
            } else {
              current_id = resolve_current_after_switch(filtered, current_id);
              images = std::move(filtered);
              active_console_filter = console_result.criterion;
            }
          }
        }
      }
      prefetch.set_current(images, current_id);
    }

    // F-24 会话续点：退出时把当前浏览到的那张写回,下次 open 从这里续上。只
    // 在干净退出(q/EOF)时写一次,不在每次导航时写——守住零延迟、每键不额外
    // IO;崩溃/被 kill 丢的只是本次位置,退回上次干净退出点,可接受。
    pzt::core::set_last_image_id(*id, current_id);

    // 退出前显式删掉最后一帧的 placement——AltScreen 切回主屏幕缓冲区、
    // 甚至用户手动跑 `clear`,都清不掉 Kitty 协议画出来的图片,那是叠加在
    // 文字网格之上的独立层,只有协议自己的 delete 命令能清。程序马上就要
    // 退出了,这一步失败没有可行的补救动作，显式 (void) 丢弃(F-19)。
    (void)pzt::cli::kitty::clear_placement(STDOUT_FILENO, mode, kImageId);
  }  // AltScreen/CbreakMode 析构,自动还原终端设置

  if (latency_count > 0) {
    std::sort(latency_samples.begin(), latency_samples.end());
    // F-36：最近秩法(nearest-rank)取 p95——ceil(0.95*n)-1,而不是原来直接
    // 截断 0.95*n。样本很少时(一次浏览只切十来张)截断会把 p95 压到偏低、
    // 甚至跟中位数区分不出;最近秩是百分位的标准定义,小样本下更有意义。夹
    // 到 [0,n-1] 纯防御。
    std::size_t n = latency_samples.size();
    std::size_t rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(n)));
    std::size_t p95_index = std::min(n - 1, rank > 0 ? rank - 1 : 0);
    std::fprintf(stderr, "[pzt open] key-to-render summary: n=%zu avg=%.2fms p95=%.2fms max=%.2fms\n",
                 latency_count, latency_sum_ms / static_cast<double>(latency_count),
                 latency_samples[p95_index], latency_max_ms);
  }

  std::fprintf(stderr, "%s", pzt::cli::i18n::msg_browse_exited().c_str());
  return 0;
}

}  // namespace pzt::cli::commands
