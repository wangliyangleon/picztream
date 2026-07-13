#include "cli/commands/commands.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "cli/i18n/i18n.h"
#include "cli/term/cbreak_mode.h"
#include "cli/text/text.h"
#include "cli/ui/ui.h"
#include "core/api.h"

// cmd_export 里用到 expand_home_path(cli/text),用 using-directive 让搬过
// 来的函数体保持逐字不变(.cpp 里用 using,头文件里绝不用)。print_usage
// 和各 cmd_* 是 public(commands.h 声明),其余 helper 只在本文件里用。
using namespace pzt::cli::text;

namespace pzt::cli::commands {

void print_usage() {
  std::fprintf(stderr, "%s", pzt::cli::i18n::usage_main().c_str());
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

// M4：headless 命令(`pzt images`/`eval`/`dedup`/`tag apply`/
// `export-images`)统一的 JSON 输出约定——成功一个 JSON 对象打到 stdout
// (换行结尾)，失败非零退出 + stderr 一行 JSON 错误对象，见
// docs/M4_Eng_Design.md"headless 命令面设计"一节。这些命令是给
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

// resolve_project 的 headless 版本：找不到项目时走 JSON 错误，不是
// i18n 人读文案。
std::optional<pzt::core::ProjectId> resolve_project_json(const std::string& project_name) {
  auto id = pzt::core::find_project_by_name(project_name);
  if (!id) {
    emit_json_error("project_not_found", "project not found: " + project_name);
  }
  return id;
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
        item["passes_gate"] = pzt::core::passes_gate(*info->evaluation);
        item["overall_score"] = pzt::core::overall_score(*info->evaluation);
      }
    }
    nlohmann::json tag_names = nlohmann::json::array();
    for (const auto& t : pzt::core::tags_for_image(r.id)) tag_names.push_back(t.name);
    item["tags"] = tag_names;
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
  std::printf("\r%s", pzt::cli::i18n::msg_export_raw_progress(done, total).c_str());
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
  std::vector<std::string> positional;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--support-raw") {
      support_raw = true;
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

  auto result = pzt::core::create_project(name, folder_path, support_raw, print_scan_progress);
  if (!result.ok()) {
    switch (result.error()) {
      case pzt::core::CreateProjectError::NameAlreadyExists:
        std::fprintf(stderr, "%s", pzt::cli::i18n::err_new_name_exists(name).c_str());
        break;
      case pzt::core::CreateProjectError::NoImagesFound:
        std::fprintf(stderr, "%s", pzt::cli::i18n::err_new_no_images(folder_path).c_str());
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
      std::printf("%s", pzt::cli::i18n::msg_project_created(p.name, p.root_path, p.image_count).c_str());
      return maybe_open_after_new(p.name);
    }
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
    std::printf("%s", pzt::cli::i18n::msg_project_item(p.name, p.image_count, p.root_path, p.archived).c_str());
  }
  return 0;
}


int cmd_archive(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_archive_missing_name().c_str());
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  auto id = pzt::core::find_project_by_name(name);
  if (!id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_project_not_found("pzt archive", name).c_str());
    return 1;
  }
  if (!pzt::core::archive_project(*id).ok()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_archive_failed(name).c_str());
    return 1;
  }
  std::printf("%s", pzt::cli::i18n::msg_project_archived(name).c_str());
  return 0;
}

int cmd_delete(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_delete_missing_name().c_str());
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
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

int cmd_export(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_missing_args().c_str());
    print_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt export", args[0]);
  if (!project_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[1]);
  if (!tag_id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_tag_not_found(args[1]).c_str());
    return 1;
  }
  std::string output_folder = expand_home_path(args[2]);

  // F-26：默认排除废片/重复，除非目标标签本身就是废片/重复，或者用户
  // 在 Settings 里显式打开了 export_reject/export_dup。
  auto settings = pzt::core::load_settings();
  auto result = pzt::core::export_tag(*tag_id, output_folder, print_export_progress,
                                       settings.export_reject, settings.export_dup);
  if (!result.ok()) {
    if (result.error() == pzt::core::ExportTagError::IoError) {
      std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_io_error(output_folder).c_str());
    } else {
      std::fprintf(stderr, "%s", pzt::cli::i18n::err_export_tag_not_found(args[1]).c_str());
    }
    return 1;
  }

  const auto& r = result.value();
  if (r.exported_count == 0 && r.skipped.empty()) {
    std::printf("%s", pzt::cli::i18n::msg_export_no_images(args[1]).c_str());
    return 0;
  }
  std::printf("%s", pzt::cli::i18n::msg_export_success(r.exported_count, output_folder, r.created_output_folder).c_str());
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
// 单独标"[已删除]",直接复用 M0 pzt list 展示归档项目的既有模式。
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
        std::printf("%s", pzt::cli::i18n::msg_recipe_version_item(v++, name, ver.highlights, ver.shadows, ver.wb_shift_r, ver.wb_shift_b).c_str());
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

  std::fprintf(stderr, "%s", pzt::cli::i18n::err_recipe_unknown_subcommand(verb).c_str());
  print_recipe_usage();
  return 1;
}

}  // namespace pzt::cli::commands
