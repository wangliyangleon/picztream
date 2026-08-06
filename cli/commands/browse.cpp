#include "cli/commands/commands.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
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

// T-10：一次性的会话提示("你的终端可能不显示图片"、"这张渲染失败了")。
// 跟 status_override 是平行的两套,notice 不置 showing_status、不拼"按任
// 意键继续"、不吃按键。
//
// 不复用 status_override 有两个原因,第二个是硬的:
// 1. 它会把下一次按键整个吃掉当"消除提示"用(见 showing_status 的注释),而
//    notice 是"顺带告诉你一声",不该打断选片;
// 2. 渲染失败不是一次性事件 - 终端不对时它每帧都会复发。每帧置
//    status_override 会让每次按键都只用于消除提示、然后重画又失败又置上,
//    用户永远无法导航,那比原来的缺陷更糟。
struct SessionNotice {
  std::string banner;  // banner 第二行画一帧,必须塞得进 content_cols(见 i18n)
  std::string detail;  // 退出、终端还原之后再打一遍的完整版;空则不打
};

// 有些文案原本是 fprintf 到 stderr 的,自带结尾换行。进 banner 之前必须去
// 掉:一个裸换行写进那一行会把边框顶乱。文案本身不动(B.1 只改投递路径)。
std::string without_trailing_newline(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

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
// key，不是经过设计的选择(docs/history/M3_PRD.md"风险与待确认问题"一节的
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

// 顶层 `e` 键:直接导出当前正在看的这一张,不需要先建标签。流程沿用当年
// filter_menu.cpp 里 `g e` 导出那一套结构(读路径 -> 校验空 ->
// expand_home_path -> 调导出 -> 拼状态文案),那个函数随 `g e` 入口一起退
// 休了,同样的结构现在由这个函数和下面的 handle_export_filtered_flow 各
// 持一份,但进度回调不能用 cmd_export
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
    write_stdout(pad_to(pzt::cli::i18n::msg_export_progress(done, total), content_cols));
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

// 点 2：`e` 二级菜单里的 `f`——导出当前 active filter 范围(f 层 ∘ 二级
// 筛选叠加之后 cmd_open 手上的 images，不是某个具体标签)。include_
// reject/include_dup 由调用方(cmd_open)算好传进来——"当前筛选本身就
// 是废片/重复"这个对称例外只有调用方知道(既可能来自 f 层标签、也可
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
    write_stdout(pad_to(pzt::cli::i18n::msg_export_progress(done, total), content_cols));
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

// --debug 面板的绘制。抽成函数是因为有两个调用方:主循环每帧画一次(正常
// 路径)，以及下面 LiveDebugPanel 在阻塞命令期间从后台线程画。每条原始日
// 志先按显示宽度换行展开，再对展开后的结果取最后 rows 行——一条长日志
// (比如完整的 AI 请求/响应)会占多行显示，不是硬截断成一行看不全。
void draw_debug_panel(const std::vector<std::string>& lines, int top_row, int start_col,
                      int content_cols, int rows) {
  std::vector<std::string> display_rows;
  for (const auto& line : lines) {
    auto wrapped = wrap_text(line, static_cast<std::size_t>(content_cols));
    display_rows.insert(display_rows.end(), wrapped.begin(), wrapped.end());
  }
  std::size_t begin = display_rows.size() > static_cast<std::size_t>(rows)
                          ? display_rows.size() - static_cast<std::size_t>(rows)
                          : 0;
  for (int i = 0; i < rows; ++i) {
    move_cursor(top_row + i, start_col + 1);
    std::size_t idx = begin + static_cast<std::size_t>(i);
    write_stdout(pad_to(idx < display_rows.size() ? display_rows[idx] : "", content_cols));
  }
}

// 把 debug 面板的几何信息带进阻塞命令里。log==nullptr 表示没开 --debug,
// 这条路径上什么都不做。
struct LiveDebugContext {
  pzt::cli::term::DebugLogRedirect* log = nullptr;
  int top_row = 0;
  int rows = 0;
};

// 阻塞命令期间让 debug 面板保持实时刷新。
//
// 平时 debug 面板靠主循环每帧重画,但 `/dedup` 是同步阻塞的——主循环停在
// 那儿,日志早就进了环形缓冲区却没人画,用户要等整条命令跑完才一次性看到
// 几十条。AI 比较尤其难受:prompt 是在 http_post **之前**打的(见
// core/ai/ai.cpp),本来正好可以让人看着"这一次在比哪两张",结果要等响应
// 回来之后才显示。
//
// 做法是在这段时间里把重画挂到 DebugLogRedirect 的读线程上:有新日志进来
// 就立刻画一次。出作用域摘掉——常挂着的话，主循环画整帧时(含 Kitty 图片
// 传输那种大块转义序列)会跟它抢 stdout，插进去半行就能把图片写坏。
//
// with_lock 是给同一段时间里主线程自己的绘制用的(进度条那两行)。两个写
// 终端的线程必须串起来,否则一条 move_cursor 和另一条的内容会交错。
class LiveDebugPanel {
 public:
  LiveDebugPanel(const LiveDebugContext& ctx, int start_col, int content_cols) : log_(ctx.log) {
    if (!log_) return;
    log_->set_on_lines_appended([this, ctx, start_col, content_cols](
                                      const std::vector<std::string>& lines) {
      std::lock_guard<std::mutex> lock(mu_);
      draw_debug_panel(lines, ctx.top_row, start_col, content_cols, ctx.rows);
    });
  }

  ~LiveDebugPanel() {
    // set_on_lines_appended(nullptr) 会等正在跑的那一次回调返回,所以这一
    // 行之后 mu_ 和 this 都不会再被读线程碰——不然就是 use-after-free。
    if (log_) log_->set_on_lines_appended(nullptr);
  }

  LiveDebugPanel(const LiveDebugPanel&) = delete;
  LiveDebugPanel& operator=(const LiveDebugPanel&) = delete;

  template <typename Fn>
  void with_lock(Fn&& fn) {
    std::lock_guard<std::mutex> lock(mu_);
    fn();
  }

 private:
  pzt::cli::term::DebugLogRedirect* log_;
  std::mutex mu_;
};

// `/dedup <范围> [--ai]`，近似重复检测唯一的触发入口，见
// docs/history/M3_Dedup_Eng_Design.md"控制台命令"一节与 docs/history/Dedup_AI_Console_PRD.md。
// 范围写法(`*` / `#标签名` / `#"带空格的标签名"`)由 take_scope_token 切
// 出来，跟 `/ai_eval` 分离"范围"和"额外指引"用的是同一个原语，引号处理
// 不需要在这里重新实现一遍。范围后面除了 `--ai` 不接受任何东西——认不出
// 来的 token 报用法错误，不当成标签名吞掉，控制台一贯"显式标记，不猜"。
//
// 整个过程是阻塞的:不开 --ai 时是本地分组，几秒到几十秒;开了 --ai 之后
// 每簇还要发 N-1 次网络比较，量级拉到分钟级。刻意不做成异步(见
// docs/history/M3_Dedup_PRD.md 与 docs/history/Dedup_AI_Console_PRD.md 两处"非目标")，
// 但两段都接了进度回调在状态栏原地重画——早年那句"主循环没有机会重绘，
// 传了也没地方画"其实不成立，handle_export_current_flow 早就在用
// move_cursor + pad_to + write_stdout 这套写法了。
//
// provider 只读 Settings.ai_provider，控制台不暴露内联的 provider 参
// 数:交互侧从来不在命令行里选模型(`/ai_eval` 同样如此)，要换 provider
// 改 config.json，跟时间窗/哈希阈值走 Settings 是同一个约定(F-08)。
//
// M3 时期这里还有一道"N 张照片还没评估，保留判断会退化成按拍摄时间选"
// 的 y/N 确认，W2026-07-21 目标二之后删掉了:那一轮改造把 keep_id 的选
// 择从"比评估分数"改成了"留 captured_at 最新"这个不依赖任何评估结果的
// 确定性基线(见 core/dedup/dedup.h 的说明)，同一轮里 overall_score/
// passes_gate 也已从下游移除。不开 AI 时本来就恒定按拍摄时间选，不存在
// "退化"这回事，那句提示只会误导用户先去跑一遍用不上的评估。现在这个位
// 置上的确认是另一回事:只在 --ai 时问，问的是"要不要为此发 M 次请求"，
// 而且是在本地分组跑完、拿到精确开销之后才问的。
std::string handle_dedup_command(pzt::core::ProjectId project_id, const std::string& rest,
                                  int banner_row, int start_col, int content_cols,
                                  const LiveDebugContext& debug_ctx) {
  auto [scope, tail] = pzt::cli::text::take_scope_token(rest);
  bool ai_enabled = false;
  if (tail == "--ai") {
    ai_enabled = true;
  } else if (!tail.empty()) {
    return pzt::cli::i18n::err_dedup_bad_args();
  }

  auto resolved = resolve_console_scope(project_id, scope);
  if (!resolved.error_message.empty()) return resolved.error_message;

  // F-12/F-26：一次读全,时间窗/哈希阈值(F-08)和废片排除开关都来自
  // 同一份 Settings,现读不缓存,跟 resolve_ai_provider() 同一个先例。
  auto settings = pzt::core::load_settings();
  if (!settings.dedup_reject) {
    exclude_scope_by_tag(resolved,
                          pzt::core::find_tag_by_name(project_id, pzt::core::tagging::kRejectTagName));
  }

  // 两段进度共用 banner 同一行,后写的覆盖先写的:分组跑完之后 AI 那段接
  // 着往同一个位置写,不会堆出两行。
  // --debug 时让 debug 面板在这条命令阻塞期间保持实时刷新,见 LiveDebugPanel
  // 的说明。下面所有往终端写的动作都走 live.with_lock，跟后台读线程的重画
  // 串起来。
  LiveDebugPanel live(debug_ctx, start_col, content_cols);
  auto draw_banner = [&](const std::string& text) {
    live.with_lock([&] {
      move_cursor(banner_row, start_col + 1);
      write_stdout(pad_to(text, static_cast<std::size_t>(content_cols)));
    });
  };
  // banner 第二行:平时留空(见 cmd_open 里 kBannerRows 那段说明),AI 阶段
  // 借它挂一行操作提示。
  auto draw_banner_line2 = [&](const std::string& text) {
    live.with_lock([&] {
      move_cursor(banner_row + 1, start_col + 1);
      write_stdout(pad_to(text, static_cast<std::size_t>(content_cols)));
    });
  };
  auto on_cluster_progress = [&](int done, int total) {
    draw_banner(pzt::cli::i18n::msg_dedup_cluster_progress(done, total));
  };
  auto on_ai_progress = [&](const pzt::core::dedup::AiProgress& p) {
    draw_banner(pzt::cli::i18n::msg_dedup_ai_progress(p.group_done, p.group_total, p.comparison_done,
                                                        p.comparison_total));
  };
  // 闸门:core 只负责报出精确开销并等一个 bool，"怎么问"是 cli 的事。
  // 按 y 以外任意键(含 Esc)= 整条命令中止，core 那边保证这种情况下一个
  // 标签都不会写，所以这里不需要任何回滚。
  pzt::core::dedup::AiGateFn on_ai_gate = nullptr;
  if (ai_enabled) {
    on_ai_gate = [&](const pzt::core::dedup::AiCost& cost) {
      // 闸门要读键,不能整段持锁(会把后台重画卡到用户按键为止)。只锁住
      // 画提示这一下,读键本身在锁外。
      char c = prompt_and_read_key_2line(
          pzt::cli::i18n::msg_dedup_ai_confirm_line1(cost.group_count, cost.comparison_count),
          pzt::cli::i18n::msg_dedup_ai_confirm_line2(), banner_row, start_col, content_cols);
      // 自己重写第二行。space/f/r 那几个二级菜单不需要这一步,是因为它们
      // 返回主循环后立刻整屏重绘;闸门返回后直接进了 core 的阻塞调用,主
      // 循环要等整个 dedup 跑完才有机会重画。不重写的话"按 y 继续,其它键
      // 取消"会一直挂在进度下面,读起来像还在等按键。
      //
      // 同意就换成操作提示,拒绝就擦干净——拒绝时整条命令立刻返回主循环,
      // 留着提示会闪一下再被整屏重绘冲掉。
      bool accepted = (c == 'y' || c == 'Y');
      draw_banner_line2(accepted ? pzt::cli::i18n::msg_dedup_ai_progress_hint() : "");
      return accepted;
    };
  }

  // 取消作用域：这一整条命令期间 Ctrl-C 表示"取消这次去重"，出了作用域
  // (不管从哪条路径出去)立刻恢复成 signal_restore 的默认语义，也就是
  // T-9a 的"还原终端后干净退出 pzt"。析构漏跑的话 Ctrl-C 会在浏览界面里
  // 彻底失效——既不取消也不退出，所以是 RAII。
  //
  // 回显那行字必须在这里就渲染好：按下 Ctrl-C 的那一刻主线程正阻塞在
  // curl 或解码里，没人能替它重画，只能由信号处理函数自己 write() 出去，
  // 而处理函数里不能调 i18n、不能分配内存。光标定位序列也要自己拼——
  // move_cursor 是直接往 stdout 写的，拿不到字符串。
  //
  // 两行一起写:第一行换成"正在取消…"，第二行那句"Ctrl-C 取消"必须同时擦
  // 掉——留着会读成"再按一次可以取消这次取消"。一次 write 写完两行，中间
  // 不会被别的绘制插进来。
  auto move_to = [&](int row) {
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(start_col + 1) + "H";
  };
  std::string cancel_echo =
      move_to(banner_row) + pad_to(pzt::cli::i18n::msg_dedup_cancelling(),
                                    static_cast<std::size_t>(content_cols)) +
      move_to(banner_row + 1) + pad_to("", static_cast<std::size_t>(content_cols));
  pzt::cli::term::signal_restore::CancelScope cancel_scope(cancel_echo);
  auto on_cancel = [&] { return cancel_scope.cancelled(); };
  // 提示从这里就挂上，不是等闸门通过之后——分簇阶段同样可取消(不带
  // `--ai` 时那是唯一的阻塞段)，只在 AI 段提示的话用户不会知道前半段也能
  // 停。闸门会临时用第二行问话，答完再把这行提示写回去。
  draw_banner_line2(pzt::cli::i18n::msg_dedup_ai_progress_hint());

  // provider/local_config 直接取上面那份 settings，不再调
  // resolve_ai_provider()/resolve_local_model_config()——那两个 helper 各
  // 自会再 load_settings() 读一次盘，而我们手上这份就是这一次按键刚读
  // 的，"现读不缓存"的语义一样满足，不必为此多读两遍。on_ai_progress 无
  // 条件传：core 只在 ai_enabled 时才会调它(见 cluster_and_choose_impl)。
  auto result = pzt::core::find_and_tag_duplicates(
      project_id, resolved.image_ids, settings.dedup_time_window_seconds, settings.dedup_hash_threshold,
      on_cluster_progress, ai_enabled, settings.ai_provider,
      pzt::core::LocalModelConfig{settings.ollama_base_url, settings.ollama_model},
      std::move(on_ai_gate), on_ai_progress, on_cancel);
  // F-25：这一步可能冻结了几秒到几十秒，期间用户习惯性按的键留在 tty
  // 缓冲区里——不清掉的话，接下来继续读键时会一次性回放，可能连按出
  // 误标签/误退出。见 docs/history/M3_Dedup_PRD.md"阻塞期间的输入缓冲行为"那
  // 条一直没收口的风险。开了 --ai 之后阻塞更久，这一步更要紧。
  flush_pending_input();
  if (!result.ok()) return pzt::cli::i18n::err_dedup_failed();
  // 中途取消跟闸门被拒分开报:闸门被拒是"没点头"，静默返回就够了;取消是
  // "点了头、跑了一阵又喊停"，已经花掉时间和 token，用户需要一句确认它真
  // 的停了、而且没留下半截结果。
  if (result.value().cancelled) return pzt::cli::i18n::msg_dedup_cancelled();
  // 闸门被拒 = 用户主动取消，静默返回,不报"找到 0 组"(那会读成"跑完了但
  // 什么都没找到"，跟实际发生的事完全不是一回事)。跟 read_text_line 按
  // Esc 取消同一个约定。
  if (result.value().ai_declined) return "";
  return pzt::cli::i18n::msg_dedup_result(result.value().group_count, result.value().tagged_count,
                                           result.value().skipped_no_capture_time,
                                           result.value().ai_fallback_count);
}

// `/ai_eval * | #标签名 [额外指引]`——批量提交，见
// docs/history/M3_PRD.md"批量评估与任务状态"一节。已经评估过的直接跳过，不重
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
  return pzt::cli::i18n::msg_ai_tasks_status(status.queued, status.processing, status.failed);
}

// F-09：控制台 `/filter <criterion>` 二级筛选——在当前 f 筛选结果之上
// (没有 f 筛选时就是全项目)再筛一层，不是 f 菜单的第三种选项，可以
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
// (跟 f 键"handle_f_key_prompt 只返回意图，cmd_open 自己算"同一个既
// 有模式)，base 是当前 f 层的结果(cmd_open 的 f_filtered_images)。
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
                                                int content_cols, const LiveDebugContext& debug_ctx) {
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
    return ConsoleCommandResult{
        handle_dedup_command(project_id, rest, banner_row, start_col, content_cols, debug_ctx)};
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
    // 默认 Local)。交互式切换 UI 本来就是 docs/history/M3_PRD.md 明确留到以后
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
                                            int content_cols, const LiveDebugContext& debug_ctx) {
  auto input = read_text_line_with_placeholder(pzt::cli::i18n::msg_ai_prompt_placeholder(),
                                                 banner_row, start_col, content_cols);
  if (!input) return ConsoleCommandResult{};  // Esc,静默取消
  if (input->empty() || (*input)[0] != '/') {
    return ConsoleCommandResult{pzt::cli::i18n::msg_console_requires_slash()};
  }
  return handle_ai_console_command(evaluation_worker, project_id, image_id, *input, banner_row,
                                    start_col, content_cols, debug_ctx);
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
  // F-09：f 层筛选结果的影子副本——`images` 本身继续驱动导航/渲染/
  // prefetch 不变,`f_filtered_images` 只在 f 切换筛选时同步更新,供
  // `/filter` 在它之上再筛一层、`/filter clear` 时还原用,见下面 `g`
  // 键处理和 `:` 键处理的说明。
  auto f_filtered_images = images;

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

  // T-10：本次会话攒下的一次性提示。banner 版一帧一条、显示过就出队;
  // detail 版在退出、终端还原之后统一再打一遍。
  //
  // 为什么退出后还要再打一遍:banner 那一条按第一个键就没了,容易错过;而
  // 且真正要用户照着做的内容(装 Ghostty、怎么关掉这句)有 160+ 列,塞不进
  // banner 的 54 列(A.2)。退出后打在真实终端上没有这个限制,能自然折行。
  // 落点必须在下面那个 block 结束之后 - DebugLogRedirect 活着的时候 stderr
  // 是被吞掉的(不管开没开 --debug),那正是 err_open_render_failed 这两条文
  // 案一直没人看得见的原因(B.1)。
  std::vector<SessionNotice> notices;
  std::size_t next_notice_to_show = 0;
  // B.1：渲染/解码失败每种一次会话只报一次,见入队处的理由。
  bool render_failure_reported = false;
  bool decode_failure_reported = false;

  // T-10 (a)：终端不在 Kitty 协议白名单里,进备用屏幕之前先说一声,等一次
  // 按键再进去。
  //
  // 这里等按键是 2026-07-30 真机验收(Terminal.app)之后改的,原来是画进
  // banner、不拦人。那条路送不到人眼前:banner 画在图片序列之前,而不认识
  // Kitty 协议的终端会把紧随其后的 APC 序列整段当普通文本打出来,几百字节
  // base64 一换行就把画面顶上去,banner 连同边框一起被滚掉。恰恰在这条提示
  // 最该出现的终端里,banner 是最不可能被看见的位置。
  //
  // 仍然不阻止进入:按任意键就继续,判定毕竟只是按环境变量猜的。确信自己终
  // 端没问题的用户把 warn_unsupported_terminal 设成 false,提示和这道等待
  // 一起消失。Ctrl-C 在这里是干净退出(ISIG 保留,终端还原走 T-9a 那条信号
  // 路径)。
  if (!mode.kitty_support_likely && settings.warn_unsupported_terminal) {
    std::fprintf(stderr, "%s\n\n%s\n", pzt::cli::i18n::warn_terminal_detail().c_str(),
                 pzt::cli::i18n::msg_press_any_key_or_ctrl_c().c_str());
    // 只为读这一个键临时切 cbreak;AltScreen 还没构造,这句提示留在主屏幕
    // 缓冲区里,退出备用屏幕之后用户还能再看到它,不需要另外重打一遍。
    pzt::cli::term::CbreakMode gate_cbreak;
    (void)pzt::cli::ui::read_one_byte();
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

    // increment 6.4.6:当前是否在 f + 数字切出来的筛选视图里,以及筛选到
    // 了哪个标签——跟 current_id 一样是这个函数作用域内的纯局部状态。
    std::optional<pzt::core::TagId> active_filter_tag_id;
    std::string active_filter_tag_name;
    // F-09：控制台二级筛选是否生效,以及是哪个条件——切 f 筛选(应用或
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
        draw_debug_panel(debug_log.snapshot(), debug_top_row, start_col, content_cols, kDebugRows);
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
        std::string trimmed = status_override;
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        write_stdout(pad_to(pzt::cli::i18n::msg_press_any_key_to_continue(trimmed), content_cols));
      } else {
        write_stdout(pad_to(pzt::cli::i18n::nav_bar_line1(), content_cols));
      }
      // 第二行:有没显示过的 session notice 就先让给它,一帧一条,显示过即
      // 出队(T-10)。notice 刻意不走 status_override:那条路会置
      // showing_status,把下一次按键整个吃掉当"消除提示"用,而 notice 是
      // "顺带告诉你一声",不该打断选片。B.1 的渲染失败提示更要紧 - 那个每
      // 帧都会复发,走 status_override 会让每次按键都只用于消除提示,用户
      // 永远无法导航。没有 notice 时:有状态提示则留空(状态提示本身是单
      // 行),否则回到常驻的导航第二行。
      move_cursor(banner_row + 1, start_col + 1);
      if (next_notice_to_show < notices.size()) {
        write_stdout(pad_to(notices[next_notice_to_show++].banner, content_cols));
      } else if (showing_status) {
        write_stdout(pad_to("", content_cols));
      } else {
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
          if (!rendered.ok() && !render_failure_reported) {
            // B.1：以前这里是 fprintf 到 stderr,而 DebugLogRedirect 默认把
            // stderr 整个丢掉,这条文案在默认路径上永远看不见。改走 notice
            // 通道。
            //
            // 一次会话只报一次:渲染失败不是一次性事件,终端不对时每换一张
            // 图就会再失败一次,不设闸门就是每帧刷屏。而且 notice 是在这一
            // 帧的 banner 画完之后才入队的(banner 先画、图片后画),所以它显
            // 示在下一帧;真的是最后一帧才失败的话,退出后那次重打兜底。
            render_failure_reported = true;
            std::string text = without_trailing_newline(pzt::cli::i18n::err_open_render_failed());
            notices.push_back({text, text});
          }
        } else if (!decode_failure_reported) {
          decode_failure_reported = true;
          std::string text = without_trailing_newline(pzt::cli::i18n::err_open_decode_failed());
          notices.push_back({text, text});
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
      // T-3：认不出来的可打印键不再被完全静默地吃掉。原来的行为下,按一个
      // 不支持的键(最典型的是 g,README 与 usage 长期写着筛选是 g,实际是
      // f)得到的是零反馈,用户分不清"卡住了"还是"按错了"。二级菜单层早就
      // 有"无效按键给一句提示"的约定(filter_menu.cpp、handle_r_key),只有
      // 顶层没有。
      char unknown_key = 0;
      while (true) {
        // T-23：这条检查刻意放在 stdin_ready 阻塞读键之前、不依赖
        // has_pending()/poll_active。原因：has_pending() 只有在 worker
        // 还没处理完排队里的东西时才是 true，而"连不上本地 Ollama"这类
        // 失败是瞬时的(连接被拒绝，不会真的发出请求、不会等超时)。一
        // 批评估提交后，worker 线程可能在主循环第一次算出 poll_active
        // 之前就已经把整批处理完、in_flight_/queue_ 都清空了 - 这种情
        // 况下 poll_active 从一开始就是 false，直接掉进下面"纯浏览态"
        // 分支(只关心 resize)，永远没有机会去看 evaluation worker 的
        // 状态，状态栏因此永远不出现，真机反馈只能靠 --debug(它是后台
        // 线程直接写 stderr，跟这整套轮询逻辑无关)才看得到。这里不管这
        // 一轮打不打算轮询 AI 状态，先无条件查一遍：便宜(一次带锁的
        // optional 判断)，查到就立刻跳出去重画，还没读 stdin 不会吞任
        // 何按键；查不到时开销可以忽略，跟以前一样往下走。
        if (auto failure = evaluation_worker.take_failure_report()) {
          // 报文件名而不是裸数据库 ID。查库只在真的失败了才走一次(不
          // 是每轮 poll)，量级可以忽略；查不到就传 nullopt，由 i18n 回
          // 落到 ID。
          auto info = pzt::core::get_image(failure->last_image_id);
          std::optional<std::string> file_name;
          if (info) file_name = info->file_name;
          status_override = pzt::cli::i18n::msg_ai_evaluation_failed(
              file_name, failure->last_image_id, failure->last_error, failure->total_failed);
          timed_out = true;
          break;
        }

        bool poll_active = debug_mode || evaluation_worker.has_pending();
        int poll_ms = poll_active ? 300 : 250;  // 纯浏览态 250ms 仅用于察觉 resize
        if (!stdin_ready(poll_ms)) {
          if (poll_active) {
            // F-03：确认拿到新结果(不是超时空转)才值得重画 - 上面那段
            // 已经处理完失败的情况，这里只剩"有没有新的成功结果落地"要
            // 看(debug 模式下重画由面板日志驱动，不需要这次确认)。
            if (!debug_mode && !evaluation_worker.consume_new_result(ai_last_seen_generation)) {
              continue;
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
        if (c == 0x1B) {
          // 方向键/功能键是 "\x1b[..." 这样的多字节序列,前缀跟裸 Esc 撞
          // 车。照 cli/ui/ui.cpp::read_line_edit_step 的既有约定用 20ms
          // 探测消歧:紧跟着还有字节就是转义序列,整段吞掉,免得按一次方向
          // 键弹三条提示。裸 Esc 在顶层没有对应动作,静默,跟二级菜单里
          // "Esc = 取消且不提示"的语义一致。
          while (stdin_ready(20)) {
            char discard = 0;
            if (read(STDIN_FILENO, &discard, 1) <= 0) break;
          }
          continue;
        }
        if (c >= 0x20 && c < 0x7F) {
          unknown_key = c;
          break;
        }
        // 其它控制字符(Ctrl-x 之类)照旧静默吞掉:它们多半是终端或用户的
        // 组合键,不是"按错了字母"这种值得提示的误操作。
      }
      if (timed_out) {
        suppress_latency_log = true;  // 没有按键,只是刷新画面(debug 面板或者 AI 新结果),不处理导航
        continue;
      }
      if (unknown_key != 0) {
        // 跟 timed_out 一样只重画、不导航,区别是带一句提示。这里确实按了
        // 键,但没有产生任何图片切换,算不上一次 key-to-render,不记延迟。
        suppress_latency_log = true;
        status_override = pzt::cli::i18n::msg_unknown_key(unknown_key);
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
        auto menu = tags_for_menu(*id);
        // F-01：跟 space 分支同样的现查逻辑,见那边的说明。
        auto duplicate_tag_id =
            pzt::core::find_tag_by_name(*id, pzt::core::tagging::kDuplicateTagName);
        auto decision =
            handle_f_key_prompt(reject_tag_id, duplicate_tag_id, menu, banner_row, start_col, content_cols);

        if (decision.action == FKeyAction::ApplyFilter) {
          // 真机测试反馈 f + 数字筛选有明显卡顿,查出来是 image_tags 按
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
            // F-09：切到新的 f 筛选,二级筛选跟着自动清空(已跟用户确
            // 认),f_filtered_images 同步成这次的结果,供 /filter 在它
            // 之上再筛。
            f_filtered_images = images;
            active_console_filter.reset();
          }
        } else if (decision.action == FKeyAction::ClearFilter) {
          if (active_filter_tag_id) {
            auto full = pzt::core::list_images(*id);
            pzt::core::ImageId new_current = resolve_current_after_switch(full, current_id);
            images = std::move(full);
            current_id = new_current;
            active_filter_tag_id.reset();
            active_filter_tag_name.clear();
            // F-09：同上,清除 f 筛选也要清空二级筛选、同步 f_filtered_images。
            f_filtered_images = images;
            active_console_filter.reset();
          }
          // 不在筛选中时 f+f 是空操作:不查库、不提示,静默——避免每次误
          // 按 f+f 在未筛选状态下也触发一次不必要的 list_images 查询。
        } else if (decision.action == FKeyAction::Cancel) {
          // decision.status 在 Esc 时是空字符串(静默),按了个不认识的
          // 键时带一句"无效按键"提示——跟 r 键的 handle_r_key 保持一致,
          // 见 handle_f_key_prompt 的说明。
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
        // 顶层导出快捷键。二级菜单始终弹出(不再区分有没有 active
        // filter):e=当前这张,a=全部(默认排除废片/重复,跟
        // `pzt export --all-keep` 同一套语义,不受 filter 是否生效影
        // 响),f=当前筛选范围(仅 filter 生效时可选)。以前"导出任意标
        // 签"挂在 g+e 下面,交互起来很诡异,已经退休；"导出全部"和"导
        // 出某个标签组"也刻意不共用同一个快捷键,避免手滑导出了不想要
        // 的范围——这也是加了 a 选项之后单键直出不再保留的理由:多一
        // 次按键换来避免误触全量导出。
        if (current_ref) {
          highlight_active_menu_key('e', menu_lines, menu_top_row, menu_rows, info_col, info_cols);
          bool filter_active = active_filter_tag_id.has_value() || active_console_filter.has_value();
          char sub = prompt_and_read_key(pzt::cli::i18n::msg_export_submenu_prompt(filter_active),
                                          banner_row, start_col, content_cols);
          if (sub == 'e') {
            status_override = handle_export_current_flow(current_ref->id, current_ref->file_name,
                                                           banner_row, start_col, content_cols);
          } else if (sub == 'a') {
            // 全项目,不是当前筛选范围——跟 f 不同,这里没有单一目标标
            // 签,不存在"目标本身就是废片/重复"这个对称例外,直接看
            // settings。
            auto all_images = pzt::core::list_images(*id);
            status_override = handle_export_filtered_flow(*id, all_images, settings.export_reject,
                                                            settings.export_dup, banner_row,
                                                            start_col, content_cols);
          } else if (sub == 'f' && filter_active) {
            // 目标本身就是废片/重复时不排除——跟 /ai_eval、/dedup、
            // pzt export 的对称例外规则一致，只是这里"目标"可能来自
            // f 层标签，也可能来自控制台二级筛选 criterion。
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
            // 不是 Esc,也不是当前可选选项里的任何一个(含 filter 未生
            // 效时按了 f)——给一句反馈而不是完全没反应,跟这个文件里其
            // 它子菜单同样的约定。Esc 静默取消。
            status_override = pzt::cli::i18n::recipe_menu_invalid_key();
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
          // 只有开了 --debug 才把面板交给阻塞命令做实时刷新;不开时
          // log=nullptr，LiveDebugPanel 整个是 no-op。
          LiveDebugContext debug_ctx;
          if (debug_mode) {
            debug_ctx.log = &debug_log;
            debug_ctx.top_row = debug_top_row;
            debug_ctx.rows = kDebugRows;
          }
          auto console_result = handle_ai_prompt_flow(evaluation_worker, *id, current_ref->id,
                                                        banner_row, start_col, content_cols, debug_ctx);
          status_override = console_result.status;
          if (console_result.action == ConsoleCommandResult::FilterAction::Clear) {
            // 没有活跃二级筛选时是静默 no-op,跟 f+f 空筛选同一个约定。
            if (active_console_filter) {
              current_id = resolve_current_after_switch(f_filtered_images, current_id);
              images = f_filtered_images;
              active_console_filter.reset();
            }
          } else if (console_result.action == ConsoleCommandResult::FilterAction::Apply) {
            auto filtered =
                apply_console_filter(*id, f_filtered_images, reject_tag_id, console_result.criterion);
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

  // T-10：把本次会话攒下的提示再打一遍。这里已经出了上面那个 block,
  // DebugLogRedirect 析构过了、stderr 换回真实终端,写出去才真的看得见。放
  // 在退出文案之前:这是用户离开这个界面时最后读到的东西,比延迟汇总更该
  // 靠近视线落点。
  for (const auto& notice : notices) {
    if (!notice.detail.empty()) {
      std::fprintf(stderr, "%s\n", notice.detail.c_str());
    }
  }

  std::fprintf(stderr, "%s", pzt::cli::i18n::msg_browse_exited().c_str());
  return 0;
}

}  // namespace pzt::cli::commands
