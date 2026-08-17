#include "cli/commands/commands.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "pzt_version.h"  // 生成物:PZT_VERSION,见 cli/CMakeLists.txt
#include "cli/i18n/i18n.h"
#include "cli/term/cbreak_mode.h"
#include "cli/text/text.h"
#include "cli/ui/ui.h"
#include "core/ai/style.h"
#include "core/api.h"

// cmd_export 里用到 expand_home_path(cli/text),用 using-directive 让搬过
// 来的函数体保持逐字不变(.cpp 里用 using,头文件里绝不用)。print_usage
// 和各 cmd_* 是 public(commands.h 声明),其余 helper 只在本文件里用。
using namespace pzt::cli::text;

namespace pzt::cli::commands {

void print_usage() {
  std::fprintf(stderr, "%s", pzt::cli::i18n::usage_main().c_str());
}

void print_help() {
  std::printf("%s", pzt::cli::i18n::usage_main().c_str());
}

void print_version() {
  std::printf("pzt %s\n", PZT_VERSION);
}

void print_tag_usage() {
  std::fprintf(stderr, "%s", pzt::cli::i18n::usage_tag().c_str());
}

void print_recipe_usage() {
  std::fprintf(stderr, "%s", pzt::cli::i18n::usage_recipe().c_str());
}

// 找不到项目时打印统一格式的错误提示。返回 nullopt 表示调用方应该直接
// return 1。
std::optional<pzt::core::ProjectId> resolve_project(const std::string& cmd,
                                                     const std::string& project_name) {
  auto id = pzt::core::find_project_by_name(project_name);
  if (!id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_project_not_found(cmd, project_name).c_str());
  }
  return id;
}

// M4：headless 命令(`pzt images`/`dedup`/`curate`/`tag apply`/
// `export-images`)统一的 JSON 输出约定——成功一个 JSON 对象打到 stdout
// (换行结尾)，失败非零退出 + stderr 一行 JSON 错误对象，见
// docs/history/M4_Eng_Design.md"headless 命令面设计"一节。这些命令是给
// agent/ 子进程调用用的，不面向人读，跟其它 cmd_* 现有的 i18n 人读文
// 案是两套并行的输出风格，互不影响。
void emit_json(const nlohmann::json& j) {
  std::printf("%s\n", j.dump().c_str());
}

int emit_json_error(const char* code, const std::string& message) {
  nlohmann::json j = {{"error", code}, {"message", message}};
  std::fprintf(stderr, "%s\n", j.dump().c_str());
  return 1;
}

// T-8：运行期进度的带外通道。stdout 的原子性一个字节不变(跑完才写，且只
// 写一个对象)，进度走 stderr、一行一个 JSON 对象——调用方 json.loads
// (stdout) 那句话不能容忍多出任何一行，而 stderr 上本来就跑着 emit_json_
// error 的对象，是既成的结构化通道。见 docs/history/Headless_
// Observability_Eng_Design.md 决策一。
//
// 外层包一个 "progress" key 而不是平铺 phase/done/total：读取方要能一眼
// 跟错误对象分辨开，靠"有没有 progress 这个 key"比靠"有没有 error"更稳
// (将来 stderr 上再多一种带外消息时判据不用改)。
//
// phase 是必需的，因为分母中途会换尺子：cluster 数的是候选簇，compare
// 数的是比较次数，两者不同源，不带 phase 就分不清"进度推进了"和"换阶段
// 了"。stderr 上两个 phase 都写，agent 侧只用哪个是它自己的取舍。
//
// 不节流：本地管道写一行的成本可忽略，节流是下游的事(谁播报谁节流)。
// stderr 无缓冲，一次 fprintf 写完整行，保证行原子。
void emit_json_progress(const char* phase, int done, int total) {
  nlohmann::json j = {{"progress", {{"phase", phase}, {"done", done}, {"total", total}}}};
  std::fprintf(stderr, "%s\n", j.dump().c_str());
}

// 票 10：AI 真正开跑之前的**精确**开销，走跟进度同一条 stderr 带外通道，
// 一条命令最多一行。
//
// 独立的 "cost" key 而不是塞进 progress：开销不是"完成了几分之几"，硬塞
// 进 done/total 会让下游那句"分母是什么"再多一种含义。上面那段注释里
// "将来 stderr 上再多一种带外消息时判据不用改"说的就是这一刀 - 读取方按
// "有没有 cost 这个 key"分辨，progress 的解析一个字节不用动。
//
// 两个数分开而不是加成一个总数：它们的单位不同（次比较 / 张评估），也走
// 不同的耗时量级，展示层要按单位分别措辞（同 T-8 真机验收推翻"把 phase
// 压掉"的那条理由）。dedup 只报比较、evaluations 恒为 0。
void emit_json_cost(int comparisons, int evaluations) {
  nlohmann::json j = {
      {"cost", {{"comparisons", comparisons}, {"evaluations", evaluations}}}};
  std::fprintf(stderr, "%s\n", j.dump().c_str());
}

// resolve_project 的 headless 版本：找不到项目时走 JSON 错误，不是
// i18n 人读文案。
std::optional<pzt::core::ProjectId> resolve_project_json(const std::string& project_name) {
  auto id = pzt::core::find_project_by_name(project_name);
  if (!id) {
    emit_json_error("project_not_found", "project not found: " + project_name);
  }
  return id;
}

// 前向声明：完整定义(含注释)在 tag_apply 附近，cmd_curate 也要用，物理
// 位置在这里只是因为 cmd_curate 的定义处比 tag_apply 更靠前。
std::optional<pzt::core::TagId> resolve_or_create_tag(pzt::core::ProjectId project_id,
                                                        const std::string& name);

// T-16：scope 解析已经收进 core::scope，headless 与 `pzt open` 控制台共用
// 那一份（在此之前这里是有意重写的第二份，两者已经分叉：交互那份认
// `#Reject` 英文别名、这份不认，于是 `pzt dedup --scope '#Reject'` 报
// tag_not_found 而 `/dedup #Reject` 可用。收编之后这处分叉消失，headless
// 侧因此获得别名支持）。留在这一层的只有"结构化错误 -> error_code"这一步
// 映射，跟交互层映射成 i18n 文案是同一个位置的两种表现，不是重复实现。
int emit_scope_error(const pzt::core::ScopeFailure& failure) {
  switch (failure.error) {
    case pzt::core::ScopeError::TagNotFound:
      return emit_json_error("tag_not_found", "tag not found: " + failure.tag_name);
    case pzt::core::ScopeError::FilterFailed:
      return emit_json_error("filter_failed", "failed to filter by tag");
    case pzt::core::ScopeError::SystemTagNotAllowed:
      return emit_json_error("system_tag_scope",
                             "cannot dedup within a system tag: " + failure.tag_name);
    case pzt::core::ScopeError::InvalidSyntax:
      return emit_json_error("invalid_scope", "scope must be * or #tag");
  }
  return emit_json_error("invalid_scope", "scope must be * or #tag");
}

// M4：批量去重，走 Settings 的 dedup_time_window_seconds/
// dedup_hash_threshold(跟交互路径的 /dedup 同一份配置来源)——这个命
// 令本身不接受内联参数覆盖阈值，想调参改 config.json，跟交互侧的既有
// 约定一致(见 docs/history/Fix_It_Night_Review.md F-08)。
//
// W2026-07-21 目标二：新增可选 --ai --provider <gemini|claude|local>——
// 不带 --ai 时调用路径、参数、JSON 输出逐字节不变(ai_fallback_count 这
// 个 key 都不出现，不是"出现但恒为 0")；--provider 只在 --ai 出现时才
// 校验/生效，解析规则跟 cmd_curate、recipe suggest 同一套三选一。
int cmd_dedup(const std::vector<std::string>& args) {
  bool json = false;
  bool ai_enabled = false;
  std::string scope;
  std::string provider_str;
  std::vector<std::string> positional;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--json") {
      json = true;
    } else if (args[i] == "--scope") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--scope requires a value");
      scope = args[++i];
    } else if (args[i] == "--ai") {
      ai_enabled = true;
    } else if (args[i] == "--provider") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--provider requires a value");
      provider_str = args[++i];
    } else {
      positional.push_back(args[i]);
    }
  }
  if (positional.empty() || scope.empty() || !json) {
    return emit_json_error(
        "usage", "usage: pzt dedup <project> --scope <*|#tag> [--ai --provider <gemini|claude|local>] --json");
  }

  pzt::core::Provider provider = pzt::core::Provider::Local;
  if (ai_enabled) {
    if (provider_str == "gemini") {
      provider = pzt::core::Provider::Gemini;
    } else if (provider_str == "claude") {
      provider = pzt::core::Provider::Claude;
    } else if (provider_str == "local") {
      provider = pzt::core::Provider::Local;
    } else {
      return emit_json_error("usage", "--ai requires --provider <gemini|claude|local>");
    }
  }

  auto project_id = resolve_project_json(positional[0]);
  if (!project_id) return 1;

  // #27 (D-2)：跟 `pzt open` 控制台的 `/dedup` 同一条规则 —— 范围本身是系
  // 统标签时拒绝，而不是静默排空之后报"0 组"。
  auto scope_result =
      pzt::core::resolve_scope(*project_id, scope, pzt::core::SystemTagPolicy::Reject);
  if (!scope_result.ok()) return emit_scope_error(scope_result.error());
  auto resolved = std::move(scope_result.value());

  auto settings = pzt::core::load_settings();
  pzt::core::LocalModelConfig local_config{settings.ollama_base_url, settings.ollama_model};
  // 两个进度回调都无条件传：on_ai_progress 只在 ai_enabled 时才会被 core
  // 调到(见 cluster_and_choose_impl)，不需要在这里判。
  //
  // 票 10：闸门接上了，但**不阻塞** - 报完精确开销无条件返回 true 继续
  // 跑。headless 这一侧没有可以当场问的人（agent 那头的用户不在同一个时
  // 间轴上），而"同步阻塞式闸门 + 单次调用 + 异步聊天界面"三个只能取
  // 两个。取用户真能拒绝那一条：数字先送出去，取消走 agent 已有的 kill
  // 通路（Dedup 在 worker.KILLABLE_STAGES 里，SIGTERM 掉这个子进程）。
  // `pzt open` 控制台里那道阻塞式闸门（browse.cpp）不受影响，它有人可
  // 问，也仍然承诺零写入。见票 10 决策一、五。
  auto result = pzt::core::find_and_tag_duplicates(
      *project_id, resolved.image_ids, settings.dedup_time_window_seconds,
      settings.dedup_hash_threshold,
      [](int done, int total) { emit_json_progress("cluster", done, total); },
      ai_enabled, provider, local_config,
      [](const pzt::core::dedup::AiCost& cost) {
        emit_json_cost(cost.comparison_count, /*evaluations=*/0);
        return true;
      },
      [](const pzt::core::dedup::AiProgress& p) {
        emit_json_progress("compare", p.comparison_done, p.comparison_total);
      });
  if (!result.ok()) {
    return emit_json_error("dedup_failed", "dedup failed");
  }

  nlohmann::json out{{"groups", result.value().group_count},
                      {"tagged", result.value().tagged_count},
                      {"skipped_no_capture_time", result.value().skipped_no_capture_time}};
  if (ai_enabled) out["ai_fallback_count"] = result.value().ai_fallback_count;
  emit_json(out);
  return 0;
}

// M4：SkipReason 转成稳定的机读标识符，不走 i18n 人读文案(那是给
// cmd_export 那条人读命令行输出用的，跟这里的 JSON 契约是两回事)。
const char* skip_reason_str(pzt::core::SkipReason reason) {
  switch (reason) {
    case pzt::core::SkipReason::SourceMissing:
      return "source_missing";
    case pzt::core::SkipReason::DecodeFailed:
      return "decode_failed";
    case pzt::core::SkipReason::RenderFailed:
      return "render_failed";
    case pzt::core::SkipReason::EncodeFailed:
      return "encode_failed";
    case pzt::core::SkipReason::RawDecodeFailed:
      return "raw_decode_failed";
  }
  return "unknown";
}

// M4：按路径导出一批图片(不是按标签查——那是 cmd_export 的事)，供
// agent 的 Deliver Stage 用。默认排除废片/重复(Settings.export_reject/
// export_dup)，跟交互侧 cmd_export/handle_export_filtered_flow 同一份
// 规则来源(见 docs/history/Fix_It_Night_Review.md F-26)。
int cmd_export_images(const std::vector<std::string>& args) {
  bool json = false;
  std::vector<std::string> positional;
  for (const auto& a : args) {
    if (a == "--json") {
      json = true;
    } else {
      positional.push_back(a);
    }
  }
  if (positional.size() < 3 || !json) {
    return emit_json_error("usage",
                            "usage: pzt export-images <project> <image_path...> <out_folder> --json");
  }

  auto project_id = resolve_project_json(positional[0]);
  if (!project_id) return 1;

  // 最后一个位置参数是输出目录，中间的全是图片路径。
  const std::string& out_folder = positional.back();
  std::vector<pzt::core::ImageId> ids;
  ids.reserve(positional.size() - 2);
  for (std::size_t i = 1; i + 1 < positional.size(); ++i) {
    auto image_id = pzt::core::find_image_by_path(*project_id, positional[i]);
    if (!image_id) {
      return emit_json_error("image_not_found", "image not found: " + positional[i]);
    }
    ids.push_back(*image_id);
  }

  auto settings = pzt::core::load_settings();
  auto result = pzt::core::export_images(*project_id, ids, expand_home_path(out_folder), nullptr,
                                          settings.export_reject, settings.export_dup);
  if (!result.ok()) {
    return emit_json_error("io_error", "failed to export (I/O error)");
  }

  nlohmann::json skipped = nlohmann::json::array();
  for (const auto& s : result.value().skipped) {
    skipped.push_back({{"path", s.file_name}, {"reason", skip_reason_str(s.reason)}});
  }
  emit_json({{"exported", result.value().exported_count},
             {"skipped", std::move(skipped)},
             {"created_dir", result.value().created_output_folder}});
  return 0;
}

// M4：策展挑图，见 docs/history/M4_Eng_Design.md 第三节。--tag 是候选范围限定
// (可选，缺省整个项目)，跟 --apply-tag(落到入选图上的标签，可选，默
// 认"精选")是两个独立的标签概念，不要混淆——前者是"从哪些图里选"，后
// 者是"选完打什么标记"。--apply-tag 走跟 tag_apply 完全一致的惰性建普
// 通标签路径，不是系统标签，重复运行不清历史标记(见 docs/history/M4_Eng_Design.md
// 第三节 Context 里的拍板：用户想用"朋友圈"/"ins"这类自定义名字，不该
// 被强绑成固定系统标签)。
//
// W2026-07-21 目标二：新增可选 --ai --provider <gemini|claude|local>，跟
// cmd_dedup 同一套解析规则；不带 --ai 时调用路径/输出逐字节不变。
int cmd_curate(const std::vector<std::string>& args) {
  bool json = false;
  int count = 0;
  bool count_set = false;
  bool ai_enabled = false;
  std::string scope_tag_name;
  std::string apply_tag_name = "精选";
  std::string provider_str;
  // 票 08：用户这次想要什么(用途/题材偏好/叙事结构)提炼成的一段话，由
  // agent 从意图里抽出来传进来。只在开 AI 时有消费者 - 关 AI 的选择是确定
  // 性的，没有能读这段话的东西。
  std::string selection_brief;
  std::vector<std::string> positional;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--json") {
      json = true;
    } else if (args[i] == "--count") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--count requires a value");
      try {
        count = std::stoi(args[++i]);
      } catch (...) {
        return emit_json_error("usage", "--count must be an integer");
      }
      count_set = true;
    } else if (args[i] == "--tag") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--tag requires a value");
      scope_tag_name = args[++i];
    } else if (args[i] == "--apply-tag") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--apply-tag requires a value");
      apply_tag_name = args[++i];
    } else if (args[i] == "--brief") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--brief requires a value");
      selection_brief = args[++i];
    } else if (args[i] == "--ai") {
      ai_enabled = true;
    } else if (args[i] == "--provider") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--provider requires a value");
      provider_str = args[++i];
    } else {
      positional.push_back(args[i]);
    }
  }
  if (positional.empty() || !count_set || count <= 0 || !json) {
    return emit_json_error("usage",
                            "usage: pzt curate <project> --count N [--tag <name>] [--apply-tag <name>] "
                            "[--brief <text>] [--ai --provider <gemini|claude|local>] --json");
  }

  pzt::core::Provider ai_provider = pzt::core::Provider::Local;
  if (ai_enabled) {
    if (provider_str == "gemini") {
      ai_provider = pzt::core::Provider::Gemini;
    } else if (provider_str == "claude") {
      ai_provider = pzt::core::Provider::Claude;
    } else if (provider_str == "local") {
      ai_provider = pzt::core::Provider::Local;
    } else {
      return emit_json_error("usage", "--ai requires --provider <gemini|claude|local>");
    }
  }

  auto project_id = resolve_project_json(positional[0]);
  if (!project_id) return 1;

  std::optional<pzt::core::TagId> candidate_scope;
  if (!scope_tag_name.empty()) {
    auto tag_id = pzt::core::find_tag_by_name(*project_id, scope_tag_name);
    if (!tag_id) return emit_json_error("tag_not_found", "tag not found: " + scope_tag_name);
    candidate_scope = tag_id;
  }

  auto settings = pzt::core::load_settings();
  pzt::core::LocalModelConfig local_config{settings.ollama_base_url, settings.ollama_model};
  // 跟 cmd_dedup 同一套带外进度（T-8）：stdout 仍然只在最后写一个对象。
  auto result = pzt::core::curate_images(
      *project_id, candidate_scope, count, settings.curate_time_window_seconds,
      settings.curate_hash_threshold, settings.curate_preselect_multiplier, ai_enabled, ai_provider,
      local_config,
      [](int done, int total) { emit_json_progress("cluster", done, total); },
      [](const pzt::core::dedup::AiProgress& p) {
        emit_json_progress("compare", p.comparison_done, p.comparison_total);
      },
      // 闸门：跟 cmd_dedup 同一个处置（票 10 决策一、五） - 报出精确开销
      // 就继续跑，不等任何人。curate 按 SPEC §3.2 不进 TUI，这条是它唯一
      // 的入口，所以"用户可以在 AI 开跑前拒绝"这件事在这里全靠 agent 收
      // 到这行之后立刻告知 + 给可取消入口来兑现。
      //
      // 报的两个数一次给全：core 保证这个钩子一趟只被问一次（curate.cpp
      // 的 gate_consulted 把 tournament 那一路和补问那一路合起来只问一
      // 次），所以这里不需要防重。
      [](int comparison_count, int evaluation_count) {
        emit_json_cost(comparison_count, evaluation_count);
        return true;
      },
      // 票 05：评估阶段的第三个 phase。cluster/compare 数的是候选簇和比
      // 较次数，这个数的是照片张数-三者单位不同，展示层必须按 phase 分
      // 别措辞(T-8 真机验收推翻过"把 phase 压掉"的做法，见 SPEC §3.2)。
      [](int done, int total) { emit_json_progress("evaluate", done, total); },
      // 票 08：排在所有回调之后是有意的，理由见 core/curate/curate.h。
      /*on_cancel=*/nullptr, selection_brief);

  if (!result.selected.empty()) {
    auto apply_tag_id = resolve_or_create_tag(*project_id, apply_tag_name);
    if (!apply_tag_id) {
      return emit_json_error("tag_create_failed", "failed to create tag: " + apply_tag_name);
    }
    for (auto id : result.selected) {
      if (!pzt::core::add_tag(id, *apply_tag_id).ok()) {
        return emit_json_error("add_tag_failed", "failed to apply tag to selected image");
      }
    }
  }

  std::unordered_map<pzt::core::ImageId, std::string> curate_path_by_id;
  for (const auto& ref : pzt::core::list_images(*project_id)) curate_path_by_id[ref.id] = ref.file_path;

  nlohmann::json selected_paths = nlohmann::json::array();
  for (auto id : result.selected) selected_paths.push_back(curate_path_by_id[id]);

  nlohmann::json out{{"requested", result.requested},
                      {"returned", result.returned},
                      {"selected", std::move(selected_paths)}};
  if (ai_enabled) {
    out["ai_fallback_count"] = result.ai_fallback_count;
    // 票 06（PRD 决策二十一）：整批的选择与排序退化，跟上面那个"某几个簇
    // 的比较退化了"是两个信号，刻意分开报-混进同一个数字会让 agent 那句
    // "哪几组不是 AI 挑的"直接说错。两个都只在 --ai 时出现，不带 --ai 的
    // 输出逐字节不变。
    out["ai_selection_fallback"] = result.ai_selection_fallback;
  }
  // 票 07（PRD 决策十五）：文案。**没有文案时这个键整个不出现**，而不是出
  // 现一个空串 - "键在不在"是单一含义，agent 那边一句 .get("caption", "")
  // 就够，不用再判空。跟 ai_fallback_count 只在 --ai 时出现是同一个立场：不
  // 留死字段。core 保证关 AI 时它恒为空，所以这里不需要再判一次 ai_enabled。
  if (!result.caption.empty()) out["caption"] = result.caption;
  emit_json(out);
  return 0;
}

// M4：agent 读项目当前状态用——每张图的路径/评估状态/达标情况/标签，
// 一次性给全，agent 不需要为了知道"评没评过"再单独查一遍(F-07 的
// evaluated_image_ids 批量查询同一个精神)。
int cmd_images(const std::vector<std::string>& args) {
  bool json = false;
  std::vector<std::string> positional;
  for (const auto& a : args) {
    if (a == "--json") {
      json = true;
    } else {
      positional.push_back(a);
    }
  }
  if (positional.empty() || !json) {
    return emit_json_error("usage", "usage: pzt images <project> --json");
  }

  auto project_id = resolve_project_json(positional[0]);
  if (!project_id) return 1;

  auto refs = pzt::core::list_images(*project_id);
  std::vector<pzt::core::ImageId> ids;
  ids.reserve(refs.size());
  for (const auto& r : refs) ids.push_back(r.id);
  auto evaluated_ids = pzt::core::evaluated_image_ids(ids);

  nlohmann::json images = nlohmann::json::array();
  for (const auto& r : refs) {
    nlohmann::json item;
    item["path"] = r.file_path;
    bool evaluated = evaluated_ids.count(r.id) > 0;
    item["evaluated"] = evaluated;
    if (evaluated) {
      auto info = pzt::core::get_image(r.id);
      if (info && info->evaluation) {
        item["unusable"] = info->evaluation->unusable;
      }
    }
    nlohmann::json tag_names = nlohmann::json::array();
    // T-25：`tags` 是原始存储名（中文字面量），只够拿来显示。agent 要按
    // "是不是废片/重复"过滤时用它就等于在 Python 里复刻一遍 core 的字面
    // 量，core 改名或未来 i18n 化系统标签名，agent 会**静默地不过滤任何东
    // 西**——不报错，只是结果变错。所以额外给一列机读标记：稳定 ASCII 别
    // 名（`Reject`/`Duplicate`），跟 `--scope '#Reject'` 认的是同一组常量
    // （tagging::kRejectTagAlias）。
    //
    // 只标记、不替 agent 做过滤：排哪些标签是**策略**、归调用方，这是 PRD
    // #23 立的那条界（"统一的是机制不是策略"）。`pzt images` 是一个读命
    // 令，给它一个"顺便按某套规则筛一遍"的开关就是把策略搬进 core。
    //
    // `tags` 原样保留：它今天没有生产消费者按名字过滤，但删掉是无收益的破
    // 坏性改动。
    nlohmann::json system_tags = nlohmann::json::array();
    for (const auto& t : pzt::core::tags_for_image(r.id)) {
      tag_names.push_back(t.name);
      if (auto alias = pzt::core::tagging::system_tag_alias(t.name)) {
        system_tags.push_back(*alias);
      }
    }
    item["tags"] = tag_names;
    item["system_tags"] = system_tags;
    images.push_back(std::move(item));
  }

  nlohmann::json out;
  out["project"] = positional[0];
  out["images"] = std::move(images);
  emit_json(out);
  return 0;
}

// new/rescan 扫到 RAW 文件时会顺带生成预览缓存(真的要跑一遍 LibRaw 降分
// 辨率解码,不是纯文件系统扫描那么快)，用 \r 覆盖同一行打印进度，不能让
// 用户误以为卡住了。done==total 时换行，交给后面的结果消息另起一行。
void print_scan_progress(int done, int total) {
  std::printf("\r%s", pzt::cli::i18n::msg_raw_preview_progress(done, total).c_str());
  std::fflush(stdout);
  if (done == total) std::printf("\n");
}

// export 遇到 kind="raw" 的图片要跑全量解码(秒级/张),同样需要进度提示，
// 跟 print_scan_progress 是同一个 \r 覆盖写法，只是文案和触发场景不同
// (一个在 new/rescan 生成预览缓存，一个在 export 真正导出)。
void print_export_progress(int done, int total) {
  std::printf("\r%s", pzt::cli::i18n::msg_export_progress(done, total).c_str());
  std::fflush(stdout);
  if (done == total) std::printf("\n");
}

// `pzt new` 成功之后，交互终端下追问"要不要直接打开"——用户反馈过 new
// 完了还得再手动敲一遍 `pzt open <name>` 太麻烦。只在 stdin/stdout 都是
// 真实 tty 时才提示、才阻塞等按键：非交互场景（脚本里调 `pzt new`、
// stdin 被重定向）不能凭空多出一个会一直等键盘输入的阻塞点，这次直接
// 返回，行为跟以前一样。真是 tty 时任意键（不分是哪个键，包括 Esc）都
// 直接打开，不提供"取消"这个选项——这个提示本身就是"按任意键打开"，不
// 是一次需要用户明确表态的确认。
int maybe_open_after_new(const std::string& name) {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
  std::printf("%s", pzt::cli::i18n::msg_new_press_any_key_to_open().c_str());
  std::fflush(stdout);
  {
    pzt::cli::term::CbreakMode cbreak;
    pzt::cli::ui::read_one_byte();
  }
  return cmd_open({name});
}

int cmd_new(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_new_missing_name().c_str());
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  // RAW 支持默认关闭、隐藏功能，见 docs/RAW_Support.md——--support-raw 不
  // 出现在 usage_main() 里，但正常生效。不管它出现在 name 后面第几个位
  // 置，摘出来之后剩下的第一个位置参数才是 folder_path。
  bool support_raw = false;
  // M4：headless ingest 用，见下面 json 分支——跟 --support-raw 同样的
  // flag 摘取方式，不占位置参数。
  bool json = false;
  std::vector<std::string> positional;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--support-raw") {
      support_raw = true;
    } else if (args[i] == "--json") {
      json = true;
    } else if (args[i].rfind("--", 0) == 0) {
      // F-06：`--` 开头但不认识的参数(比如拼错的 --supportraw)不能静默
      // 落进 positional、被当成 folder_path——那样扫描目标会变成一个不
      // 存在的"目录",容易被误解成程序出问题而不是自己打错了参数。
      std::fprintf(stderr, "%s", pzt::cli::i18n::err_new_unknown_arg(args[i]).c_str());
      print_usage();
      return 1;
    } else {
      positional.push_back(args[i]);
    }
  }
  std::string folder_path =
      !positional.empty() ? positional[0] : std::filesystem::current_path().string();

  // --json 模式下 stdout 只能有一个 JSON 对象(headless 契约)——扫描进
  // 度这条 \r 覆盖写的人读文案不能掺进去，这里传 nullptr 直接不打。
  auto result = pzt::core::create_project(name, folder_path, support_raw,
                                           json ? pzt::core::ScanProgressFn(nullptr) : print_scan_progress);
  if (!result.ok()) {
    if (json) {
      switch (result.error()) {
        case pzt::core::CreateProjectError::NameAlreadyExists:
          return emit_json_error("name_exists", "project name already exists: " + name);
        case pzt::core::CreateProjectError::NoImagesFound:
          return emit_json_error("no_images_found", "no images found in folder: " + folder_path);
        case pzt::core::CreateProjectError::NoImagesFoundRawIgnored:
          return emit_json_error(
              "no_images_found_raw_ignored",
              "no JPEG found in folder: " + folder_path +
                  ", but RAW files are present - retry with --support-raw");
      }
    }
    switch (result.error()) {
      case pzt::core::CreateProjectError::NameAlreadyExists:
        std::fprintf(stderr, "%s", pzt::cli::i18n::err_new_name_exists(name).c_str());
        break;
      case pzt::core::CreateProjectError::NoImagesFound:
        std::fprintf(stderr, "%s", pzt::cli::i18n::err_new_no_images(folder_path).c_str());
        break;
      case pzt::core::CreateProjectError::NoImagesFoundRawIgnored:
        std::fprintf(stderr, "%s",
                     pzt::cli::i18n::err_new_no_images_raw_ignored(folder_path).c_str());
        break;
    }
    return 1;
  }

  // increment 6.4.5:项目一创建就把"废片"系统标签建好——这时候项目刚建
  // 出来,保证没有任何标签,不需要处理"同名标签已经存在但不是系统标签"
  // 这种迁移场景,pzt open 不需要再管这件事。
  pzt::core::ensure_reject_tag(result.value());

  // Look the freshly-created project back up to report its scanned image
  // count - a bit wasteful (re-queries all projects) but this is a one-shot
  // CLI invocation, not a hot path.
  for (const auto& p : pzt::core::list_projects()) {
    if (p.id == result.value()) {
      if (json) {
        emit_json({{"project", p.name}, {"image_count", p.image_count}});
        return 0;
      }
      std::printf("%s", pzt::cli::i18n::msg_project_created(p.name, p.root_path, p.image_count).c_str());
      return maybe_open_after_new(p.name);
    }
  }
  if (json) {
    emit_json({{"project", name}, {"image_count", 0}});
    return 0;
  }
  std::printf("%s", pzt::cli::i18n::msg_project_created_simple(name).c_str());
  return maybe_open_after_new(name);
}

int cmd_list(const std::vector<std::string>& args) {
  (void)args;
  auto projects = pzt::core::list_projects();
  if (projects.empty()) {
    std::printf("%s", pzt::cli::i18n::msg_project_list_empty().c_str());
    return 0;
  }
  for (const auto& p : projects) {
    std::printf("%s", pzt::cli::i18n::msg_project_item(p.name, p.image_count, p.root_path).c_str());
  }
  return 0;
}

int cmd_delete(const std::vector<std::string>& args) {
  // 先分离出 flag：--json/--force 走 agent 用的 headless 路径，跳过交互式
  // stdin 确认（AG-14：agent 清扫超龄终态 run 的 pzt 项目需要能 headless
  // 删除；交互确认在子进程里会挂死）。positional[0] 是项目名（=run_id）。
  bool json = false;
  bool force = false;
  std::vector<std::string> positional;
  for (const auto& a : args) {
    if (a == "--json") json = true;
    else if (a == "--force" || a == "--yes") force = true;
    else positional.push_back(a);
  }

  if (json) {
    if (positional.empty()) {
      return emit_json_error("usage", "usage: pzt delete <project> --force --json");
    }
    if (!force) {
      return emit_json_error("usage", "headless delete requires --force");
    }
    auto jid = pzt::core::find_project_by_name(positional[0]);
    if (!jid) {
      return emit_json_error("project_not_found", "project not found: " + positional[0]);
    }
    if (!pzt::core::delete_project(*jid).ok()) {
      return emit_json_error("delete_failed", "failed to delete: " + positional[0]);
    }
    emit_json({{"deleted", positional[0]}});
    return 0;
  }

  if (positional.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_delete_missing_name().c_str());
    print_usage();
    return 1;
  }
  const std::string& name = positional[0];
  auto id = pzt::core::find_project_by_name(name);
  if (!id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_project_not_found("pzt delete", name).c_str());
    return 1;
  }

  std::printf("%s", pzt::cli::i18n::msg_delete_warn_prompt(name).c_str());
  std::printf("%s", pzt::cli::i18n::msg_delete_confirm_input().c_str());
  std::fflush(stdout);
  std::string confirmation;
  if (!std::getline(std::cin, confirmation) || confirmation != name) {
    std::printf("%s", pzt::cli::i18n::msg_delete_cancelled().c_str());
    return 1;
  }

  if (!pzt::core::delete_project(*id).ok()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_delete_failed(name).c_str());
    return 1;
  }
  std::printf("%s", pzt::cli::i18n::msg_project_deleted(name).c_str());
  return 0;
}

// 惰性 find-or-create 一个非系统标签(不设 cap、无序)：找不到就建，建的
// 时候撞见并发/TOCTOU 导致 NameAlreadyExists(两次 headless 调用几乎同
// 时给同一个新标签名建标签)就再查一次兜底，拿第一个刚建好的 id，不当
// 成失败处理。tag_apply 和 cmd_curate 共用这一段逻辑。
std::optional<pzt::core::TagId> resolve_or_create_tag(pzt::core::ProjectId project_id,
                                                        const std::string& name) {
  auto tag_id = pzt::core::find_tag_by_name(project_id, name);
  if (tag_id) return tag_id;
  auto created = pzt::core::create_tag(project_id, name, std::nullopt, false);
  if (created.ok()) return created.value();
  return pzt::core::find_tag_by_name(project_id, name);
}

// M4：headless verb——按路径给一张图打标签，标签不存在就惰性建(非系
// 统标签、不设 cap、无序)。交互菜单里"超 cap 弹子菜单选替换谁"这个人
// 的决定，headless 没有菜单可弹，变成显式策略参数 --on-cap：fail(默
// 认)报错退出，skip 不算失败、输出 applied:false。add_tag 本身幂等
// (同一张图重复打同一个标签直接算成功)，这里不需要额外处理。
int tag_apply(const std::vector<std::string>& args) {
  bool json = false;
  std::string on_cap = "fail";
  std::vector<std::string> positional;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--json") {
      json = true;
    } else if (args[i] == "--on-cap") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--on-cap requires a value");
      on_cap = args[++i];
    } else {
      positional.push_back(args[i]);
    }
  }
  if (positional.size() < 3 || !json) {
    return emit_json_error(
        "usage", "usage: pzt tag apply <project> <image_path> <tag> [--on-cap {fail|skip}] --json");
  }
  if (on_cap != "fail" && on_cap != "skip") {
    return emit_json_error("usage", "--on-cap must be 'fail' or 'skip'");
  }

  auto project_id = resolve_project_json(positional[0]);
  if (!project_id) return 1;

  auto image_id = pzt::core::find_image_by_path(*project_id, positional[1]);
  if (!image_id) {
    return emit_json_error("image_not_found", "image not found: " + positional[1]);
  }

  auto tag_id = resolve_or_create_tag(*project_id, positional[2]);
  if (!tag_id) {
    return emit_json_error("tag_create_failed", "failed to create tag: " + positional[2]);
  }

  auto result = pzt::core::add_tag(*image_id, *tag_id);
  if (!result.ok()) {
    if (result.error().kind == pzt::core::AddTagFailureKind::CapExceeded) {
      if (on_cap == "skip") {
        emit_json({{"applied", false}, {"reason", "cap"}});
        return 0;
      }
      return emit_json_error("cap_exceeded", "tag cap exceeded: " + positional[2]);
    }
    return emit_json_error("add_tag_failed", "failed to apply tag");
  }

  emit_json({{"applied", true}});
  return 0;
}

// M4：headless verb——把某个标签从项目里当前打了它的所有图上摘掉(整
// 个项目范围，没有 --scope 参数；curate 的 --apply-tag 想要"这次是全新
// 一批"语义时，agent 自己先调这个命令清一遍再重新 curate，见
// docs/history/M4_Eng_Design.md 第三节)。标签本身不存在(从来没打过)是幂等成
// 功，cleared:0，不当错误处理——跟 remove_tag 自己"图片本来就没这个标
// 签，删除也算成功"是同一个幂等哲学，调用方不需要先查标签存不存在再
// 决定要不要清。
int tag_clear(const std::vector<std::string>& args) {
  bool json = false;
  std::vector<std::string> positional;
  for (const auto& a : args) {
    if (a == "--json") {
      json = true;
    } else {
      positional.push_back(a);
    }
  }
  if (positional.size() < 2 || !json) {
    return emit_json_error("usage", "usage: pzt tag clear <project> <tag> --json");
  }

  auto project_id = resolve_project_json(positional[0]);
  if (!project_id) return 1;

  auto tag_id = pzt::core::find_tag_by_name(*project_id, positional[1]);
  if (!tag_id) {
    emit_json({{"cleared", 0}});
    return 0;
  }

  auto tagged = pzt::core::filter_by_tag(*tag_id);
  int cleared = 0;
  if (tagged.ok()) {
    for (const auto& ref : tagged.value()) {
      if (pzt::core::remove_tag(ref.id, *tag_id).ok()) ++cleared;
    }
  }

  emit_json({{"cleared", cleared}});
  return 0;
}

int tag_list(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_tag_list_missing_name().c_str());
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag list", args[0]);
  if (!project_id) return 1;

  auto tags = pzt::core::list_tags(*project_id);
  if (tags.empty()) {
    std::printf("%s", pzt::cli::i18n::msg_tag_list_empty().c_str());
    return 0;
  }
  for (const auto& t : tags) {
    std::printf("%s", pzt::cli::i18n::msg_tag_item(pzt::cli::i18n::tag_display_name(t), t.tagged_count,
                                                    t.cap, t.is_ordered, t.is_system)
                          .c_str());
  }
  return 0;
}

int cmd_rescan(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_rescan_missing_name().c_str());
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  bool prune = true;
  // --support-raw：RAW 支持默认关闭、隐藏功能，见 docs/RAW_Support.md。
  bool support_raw = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--no-prune") {
      prune = false;
    } else if (args[i] == "--support-raw") {
      support_raw = true;
    } else {
      std::fprintf(stderr, "%s", pzt::cli::i18n::err_rescan_unknown_arg(args[i]).c_str());
      print_usage();
      return 1;
    }
  }

  auto project_id = resolve_project("pzt rescan", name);
  if (!project_id) return 1;

  auto result = pzt::core::rescan_project(*project_id, prune, support_raw, print_scan_progress);
  if (!result.ok()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_rescan_failed(name).c_str());
    return 1;
  }
  std::printf("%s", pzt::cli::i18n::msg_rescan_result(
      static_cast<long long>(result.value().added_count),
      static_cast<long long>(result.value().removed_count),
      static_cast<long long>(result.value().total_count),
      static_cast<long long>(result.value().upgraded_count)).c_str());
  return 0;
}

// export_tag 和 --all-keep 两条路径导出成功之后的结果打印是同一套(成功
// 数/跳过明细),只有"0 张"时的提示文案不同(有没有标签名可带),抽出来避
// 免两份几乎一样的打印代码分叉维护。
void print_export_result(const pzt::core::ExportResult& r, const std::string& output_folder,
                          const std::string& no_images_msg) {
  if (r.exported_count == 0 && r.skipped.empty()) {
    std::printf("%s", no_images_msg.c_str());
    return;
  }
  std::printf("%s", pzt::cli::i18n::msg_export_success(r.exported_count, output_folder,
                                                         r.created_output_folder)
                         .c_str());
  if (r.skipped.empty()) {
    std::printf("\n");
  } else {
    std::printf("%s", pzt::cli::i18n::msg_export_skipped(r.skipped.size()).c_str());
    for (const auto& s : r.skipped) {
      std::printf("%s", pzt::cli::i18n::msg_export_skipped_item(
                             s.file_name, pzt::cli::i18n::export_skip_reason(s.reason))
                             .c_str());
    }
  }
}

int cmd_export(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_missing_args().c_str());
    print_usage();
    return 1;
  }
  // --all-keep 走 flag 摘取而不是"第二个位置参数刚好等于这个字面量"——
  // 后者会把 flag 和 tag_name 挤进同一个命名空间:拼错的 --all-keeps 会
  // 静默变成一个标签名,报出来的是"找不到标签",把 flag 打错说成标签问
  // 题(F-06 在 cmd_new 记的是同一个失败模式)。摘出来之后剩下的位置参数
  // 才是 tag_name/output_folder,flag 出现在第几个位置都认。
  bool all_keep = false;
  std::vector<std::string> positional;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--all-keep") {
      all_keep = true;
    } else if (args[i].rfind("--", 0) == 0) {
      std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_unknown_arg(args[i]).c_str());
      print_usage();
      return 1;
    } else {
      positional.push_back(args[i]);
    }
  }
  // --all-keep 不占位置参数:带它时只需要 output_folder,不带时还要 tag_name。
  if (positional.size() < (all_keep ? 1u : 2u)) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_missing_args().c_str());
    print_usage();
    return 1;
  }

  auto project_id = resolve_project("pzt export", args[0]);
  if (!project_id) return 1;
  // --all-keep 时 positional[0] 就是 output_folder(没有 tag_name)。
  std::string output_folder = expand_home_path(all_keep ? positional[0] : positional[1]);

  // F-26：默认排除废片/重复，除非目标标签本身就是废片/重复(--all-keep
  // 没有单一目标标签,这条例外不适用),或者用户在 Settings 里显式打开
  // 了 export_reject/export_dup。
  auto settings = pzt::core::load_settings();

  if (all_keep) {
    auto images = pzt::core::list_images(*project_id);
    std::vector<pzt::core::ImageId> ids;
    ids.reserve(images.size());
    for (const auto& img : images) ids.push_back(img.id);
    auto result = pzt::core::export_images(*project_id, ids, output_folder, print_export_progress,
                                            settings.export_reject, settings.export_dup);
    if (!result.ok()) {
      std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_io_error(output_folder).c_str());
      return 1;
    }
    print_export_result(result.value(), output_folder, pzt::cli::i18n::msg_export_no_images_all());
    return 0;
  }

  const std::string& tag_name = positional[0];
  auto tag_id = pzt::core::find_tag_by_name(*project_id, tag_name);
  if (!tag_id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_tag_not_found(tag_name).c_str());
    return 1;
  }
  auto result = pzt::core::export_tag(*tag_id, output_folder, print_export_progress,
                                       settings.export_reject, settings.export_dup);
  if (!result.ok()) {
    if (result.error() == pzt::core::ExportTagError::IoError) {
      std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_io_error(output_folder).c_str());
    } else {
      std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_tag_not_found(tag_name).c_str());
    }
    return 1;
  }

  print_export_result(result.value(), output_folder, pzt::cli::i18n::msg_export_no_images(tag_name));
  return 0;
}

int cmd_tag(const std::vector<std::string>& args) {
  if (args.empty()) {
    print_tag_usage();
    return 1;
  }
  const std::string& verb = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  if (verb == "list") return tag_list(rest);
  if (verb == "apply") return tag_apply(rest);
  if (verb == "clear") return tag_clear(rest);

  std::fprintf(stderr, "%s", pzt::cli::i18n::err_tag_unknown_subcommand(verb).c_str());
  print_tag_usage();
  return 1;
}

// increment 2:`pzt recipe rename`/`delete` 用 "<preset_name>:<version_
// number>" 这种地址寻址一个 version——预设用名字(固定、稳定),version 用
// 该预设下的编号(排除已软删除的,按 id 升序排位,即 recipe_list 打印出来
// 的那个编号)。这纯粹是 CLI 输入约定,不是业务概念,不下沉进 core。
std::optional<std::pair<std::string, int>> parse_recipe_address(const std::string& address) {
  auto colon = address.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= address.size()) return std::nullopt;
  std::string preset_name = address.substr(0, colon);
  std::string number_part = address.substr(colon + 1);
  try {
    std::size_t consumed = 0;
    long long n = std::stoll(number_part, &consumed);
    if (consumed != number_part.size() || n <= 0) return std::nullopt;
    return std::make_pair(preset_name, static_cast<int>(n));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// 第三处需要"按名字找预设"的地方(create-debug、resolve_recipe_address
// 自己各写过一遍)，这次抽成共用的小函数。
std::optional<pzt::core::RecipeId> find_preset_by_name(const std::string& name) {
  auto presets = pzt::core::list_presets();
  auto it = std::find_if(presets.begin(), presets.end(),
                          [&](const auto& p) { return p.name == name; });
  return it == presets.end() ? std::nullopt : std::optional(it->id);
}

// decode_image_for_ai：把 image_id 解码成 DecodedImage，供 recipe suggest
// 用。复刻 core/ai/evaluation_worker.cpp 里私有的 resolve_path 逻辑(RAW
// 走 preview_cache_path，否则走 project_root/file_path)——这是一份小的、
// 可接受的重复，不为此新增 core/api 门面函数。
std::optional<pzt::core::DecodedImage> decode_image_for_ai(pzt::core::ImageId image_id) {
  auto info = pzt::core::get_image(image_id);
  if (!info) return std::nullopt;
  auto project_summary = pzt::core::open_project(info->project_id);
  if (!project_summary.ok()) return std::nullopt;

  std::string path = (info->kind == "raw" && info->preview_cache_path)
                          ? *info->preview_cache_path
                          : project_summary.value().root_path + "/" + info->file_path;
  auto decoded = pzt::core::decode_preview_file(path);
  return decoded.ok() ? std::optional(decoded.value()) : std::nullopt;
}

// M4：StyleError 转成稳定的机读标识符，同 skip_reason_str 的理由
// (headless JSON 契约，不走 i18n 人读文案)。
const char* style_error_str(pzt::core::ai::StyleError error) {
  switch (error) {
    case pzt::core::ai::StyleError::MissingApiKey:
      return "missing_api_key";
    case pzt::core::ai::StyleError::NetworkError:
      return "network_error";
    case pzt::core::ai::StyleError::HttpError:
      return "http_error";
    case pzt::core::ai::StyleError::ParseError:
      return "parse_error";
    case pzt::core::ai::StyleError::Hallucinated:
      return "hallucinated";
  }
  return "unknown";
}

// headless：LLM 看图从 9 个预设(排除 Origin)里选一个合适的风格，只
// 读，不改库——目标三 Style Stage 的"suggest"半步，"apply"半步是下面
// 的 recipe_apply，两者拆开是为了给未来"用户点名风格"的手动路径留一
// 个直接调 apply 的口子。见 docs/history/W2026-07-15_AgentStyle_Eng_Design.md。
int recipe_suggest(const std::vector<std::string>& args) {
  bool json = false;
  std::string provider_str;
  std::vector<std::string> positional;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--json") {
      json = true;
    } else if (args[i] == "--provider") {
      if (i + 1 >= args.size()) return emit_json_error("usage", "--provider requires a value");
      provider_str = args[++i];
    } else {
      positional.push_back(args[i]);
    }
  }
  pzt::core::Provider provider;
  if (provider_str == "gemini") {
    provider = pzt::core::Provider::Gemini;
  } else if (provider_str == "claude") {
    provider = pzt::core::Provider::Claude;
  } else if (provider_str == "local") {
    provider = pzt::core::Provider::Local;
  } else {
    return emit_json_error("usage",
                            "usage: pzt recipe suggest <project> <image_path> --provider "
                            "<gemini|claude|local> --json");
  }
  if (positional.size() != 2 || !json) {
    return emit_json_error("usage",
                            "usage: pzt recipe suggest <project> <image_path> --provider "
                            "<gemini|claude|local> --json");
  }

  auto project_id = resolve_project_json(positional[0]);
  if (!project_id) return 1;
  auto image_id = pzt::core::find_image_by_path(*project_id, positional[1]);
  if (!image_id) return emit_json_error("image_not_found", "image not found: " + positional[1]);

  auto decoded = decode_image_for_ai(*image_id);
  if (!decoded) {
    return emit_json_error("image_unavailable", "failed to decode image: " + positional[1]);
  }

  std::vector<std::string> preset_names;
  for (const auto& p : pzt::core::list_presets()) {
    if (p.id != 0) preset_names.push_back(p.name);
  }

  auto settings = pzt::core::load_settings();
  pzt::core::LocalModelConfig local_config{settings.ollama_base_url, settings.ollama_model};

  auto result =
      pzt::core::ai::request_style_suggestion(*decoded, preset_names, provider, local_config);
  if (!result.ok()) {
    return emit_json_error(style_error_str(result.error()), "style suggestion failed");
  }

  emit_json({{"recipe_name", result.value().recipe_name}, {"reasoning", result.value().reasoning}});
  return 0;
}

// headless：按名字把一个预设应用到一张图，纯 set_image_recipe 包壳，不
// 碰 AI——见上面 recipe_suggest 的注释。
int recipe_apply(const std::vector<std::string>& args) {
  bool json = false;
  std::vector<std::string> positional;
  for (const auto& a : args) {
    if (a == "--json") {
      json = true;
    } else {
      positional.push_back(a);
    }
  }
  if (positional.size() != 3 || !json) {
    return emit_json_error("usage",
                            "usage: pzt recipe apply <project> <image_path> <recipe_name> --json");
  }

  auto project_id = resolve_project_json(positional[0]);
  if (!project_id) return 1;
  auto image_id = pzt::core::find_image_by_path(*project_id, positional[1]);
  if (!image_id) return emit_json_error("image_not_found", "image not found: " + positional[1]);
  auto recipe_id = find_preset_by_name(positional[2]);
  if (!recipe_id) return emit_json_error("recipe_not_found", "recipe not found: " + positional[2]);

  auto result = pzt::core::set_image_recipe(*image_id, *recipe_id);
  if (!result.ok()) {
    return emit_json_error("set_recipe_failed", "failed to set recipe");
  }

  emit_json({{"applied", true}, {"recipe_name", positional[2]}});
  return 0;
}

std::optional<pzt::core::RecipeId> resolve_recipe_address(const std::string& preset_name,
                                                            int version_number) {
  auto preset_id = find_preset_by_name(preset_name);
  if (!preset_id) return std::nullopt;

  auto versions = pzt::core::list_versions(*preset_id);
  int v = 1;
  for (const auto& ver : versions) {
    if (ver.deleted) continue;
    if (v == version_number) return ver.id;
    ++v;
  }
  return std::nullopt;
}

// 预设是全局的,不属于任何项目,不需要 <project_name> 参数,跟 tag_list
// 的写法不一样。increment 2:版本编号只发给未软删除的(按 id 升序排位,
// 跟 `r` 菜单看到的编号一致,也是 pzt recipe rename/delete 寻址语法里
// <version_number> 的定义);已删除的不给编号(不再是能被寻址的目标),
// 单独标"[已删除]"。这个"行尾挂一个方括号状态后缀"的写法当初是照着 M0
// pzt list 标注归档项目来的,那个标注已随 T-32 删掉,这里是仅存的一处。
int recipe_list(const std::vector<std::string>& args) {
  if (!args.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_list_no_args().c_str());
    print_recipe_usage();
    return 1;
  }
  auto presets = pzt::core::list_presets();
  if (presets.empty()) {
    std::printf("%s", pzt::cli::i18n::msg_recipe_list_empty().c_str());
    return 0;
  }
  int i = 1;
  for (const auto& p : presets) {
    std::printf("%s", pzt::cli::i18n::msg_recipe_preset_item(i++, p.name).c_str());
    auto versions = pzt::core::list_versions(p.id);
    int v = 1;
    for (const auto& ver : versions) {
      std::string name = ver.name.value_or(pzt::cli::i18n::msg_recipe_version_unnamed_label());
      if (ver.deleted) {
        std::printf("      -   %-14s %s\n", name.c_str(), pzt::cli::i18n::msg_recipe_version_deleted_label().c_str());
      } else {
        std::printf("%s", pzt::cli::i18n::msg_recipe_version_item(v++, name, ver.highlights, ver.shadows, ver.wb_shift_r, ver.wb_shift_b, ver.contrast, ver.saturation, ver.blacks, ver.whites).c_str());
      }
    }
  }
  return 0;
}

int recipe_rename(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_rename_missing_args().c_str());
    print_recipe_usage();
    return 1;
  }
  auto address = parse_recipe_address(args[0]);
  if (!address) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_rename_invalid_address(args[0]).c_str());
    return 1;
  }
  auto id = resolve_recipe_address(address->first, address->second);
  if (!id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_rename_not_found(args[0]).c_str());
    return 1;
  }
  if (!pzt::core::rename_version(*id, args[1]).ok()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_rename_failed().c_str());
    return 1;
  }
  std::printf("%s", pzt::cli::i18n::msg_recipe_renamed(args[1]).c_str());
  return 0;
}

int recipe_delete(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_delete_missing_args().c_str());
    print_recipe_usage();
    return 1;
  }
  auto address = parse_recipe_address(args[0]);
  if (!address) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_delete_invalid_address(args[0]).c_str());
    return 1;
  }
  auto id = resolve_recipe_address(address->first, address->second);
  if (!id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_delete_not_found(args[0]).c_str());
    return 1;
  }
  if (!pzt::core::delete_version(*id).ok()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_delete_failed().c_str());
    return 1;
  }
  std::printf("%s", pzt::cli::i18n::msg_recipe_deleted(args[0]).c_str());
  return 0;
}

int cmd_recipe(const std::vector<std::string>& args) {
  if (args.empty()) {
    print_recipe_usage();
    return 1;
  }
  const std::string& verb = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  if (verb == "list") return recipe_list(rest);
  if (verb == "rename") return recipe_rename(rest);
  if (verb == "delete") return recipe_delete(rest);
  if (verb == "suggest") return recipe_suggest(rest);
  if (verb == "apply") return recipe_apply(rest);

  std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_unknown_subcommand(verb).c_str());
  print_recipe_usage();
  return 1;
}

}  // namespace pzt::cli::commands
