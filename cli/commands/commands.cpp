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

#include "cli/text/text.h"
#include "core/api.h"

// cmd_export 里用到 expand_home_path(cli/text),用 using-directive 让搬过
// 来的函数体保持逐字不变(.cpp 里用 using,头文件里绝不用)。print_usage
// 和各 cmd_* 是 public(commands.h 声明),其余 helper 只在本文件里用。
using namespace pzt::cli::text;

namespace pzt::cli::commands {

void print_usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  pzt new <project_name> [folder_path]\n"
               "  pzt list\n"
               "  pzt open [project_name] [--debug]  (h/l 上一张/下一张,"
               "j/k 下一张/上一张未打标签,space 打标签,x 标记废片,g 筛选,"
               "r 应用/清除/新建/删除风格,r v 临时预览原图,"
               "q 退出;--debug 时在图片下方开一块区域滚动显示内部日志,默认"
               "不显示也不产生这些日志)\n"
               "  pzt archive <project_name>\n"
               "  pzt delete <project_name>\n"
               "  pzt rescan <project_name> [--no-prune]  (默认会清除磁盘上已消失的"
               "文件记录,连带清掉其标签;对着可能暂时没挂载完整的存储位置跑时,"
               "加 --no-prune 跳过清理)\n"
               "  pzt export <project_name> <tag_name> <output_folder> [--link]\n"
               "  pzt tag list <project_name>\n"
               "  pzt recipe list\n");
}

void print_tag_usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  pzt tag list <project_name>\n");
}

void print_recipe_usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  pzt recipe list\n"
               "  pzt recipe rename <preset>:<version_number> <new_name>\n"
               "  pzt recipe delete <preset>:<version_number>\n");
}

// 找不到项目时打印统一格式的错误提示。返回 nullopt 表示调用方应该直接
// return 1。
std::optional<pzt::core::ProjectId> resolve_project(const std::string& cmd,
                                                     const std::string& project_name) {
  auto id = pzt::core::find_project_by_name(project_name);
  if (!id) {
    std::fprintf(stderr, "%s: 找不到项目 '%s',用 pzt list 查看可用项目\n", cmd.c_str(),
                 project_name.c_str());
  }
  return id;
}

int cmd_new(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt new: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  std::string folder_path =
      args.size() >= 2 ? args[1] : std::filesystem::current_path().string();

  auto result = pzt::core::create_project(name, folder_path);
  if (!result.ok()) {
    switch (result.error()) {
      case pzt::core::CreateProjectError::NameAlreadyExists:
        std::fprintf(stderr, "pzt new: 项目名 '%s' 已存在\n", name.c_str());
        break;
      case pzt::core::CreateProjectError::NoImagesFound:
        std::fprintf(stderr, "pzt new: '%s' 目录下没有找到任何 JPEG 文件\n",
                     folder_path.c_str());
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
      std::printf("已创建项目 '%s'(%s),共 %lld 张 JPEG\n", p.name.c_str(),
                  p.root_path.c_str(), static_cast<long long>(p.image_count));
      return 0;
    }
  }
  std::printf("已创建项目 '%s'\n", name.c_str());
  return 0;
}

int cmd_list(const std::vector<std::string>& args) {
  (void)args;
  auto projects = pzt::core::list_projects();
  if (projects.empty()) {
    std::printf("(还没有任何项目,用 pzt new 创建一个)\n");
    return 0;
  }
  for (const auto& p : projects) {
    std::printf("%-20s %8lld 张  %s%s\n", p.name.c_str(),
                static_cast<long long>(p.image_count), p.root_path.c_str(),
                p.archived ? "  [已归档]" : "");
  }
  return 0;
}


int cmd_archive(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt archive: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  auto id = pzt::core::find_project_by_name(name);
  if (!id) {
    std::fprintf(stderr, "pzt archive: 找不到项目 '%s',用 pzt list 查看可用项目\n", name.c_str());
    return 1;
  }
  if (!pzt::core::archive_project(*id).ok()) {
    std::fprintf(stderr, "pzt archive: 找不到项目 '%s'\n", name.c_str());
    return 1;
  }
  std::printf("已归档项目 '%s'\n", name.c_str());
  return 0;
}

int cmd_delete(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt delete: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  auto id = pzt::core::find_project_by_name(name);
  if (!id) {
    std::fprintf(stderr, "pzt delete: 找不到项目 '%s',用 pzt list 查看可用项目\n", name.c_str());
    return 1;
  }

  std::printf(
      "即将删除项目 '%s' 的全部标签与浏览状态,不影响磁盘上的照片文件,此操作不可撤销。\n",
      name.c_str());
  std::printf("请再次输入项目名确认删除: ");
  std::fflush(stdout);
  std::string confirmation;
  if (!std::getline(std::cin, confirmation) || confirmation != name) {
    std::printf("已取消,项目未被删除\n");
    return 1;
  }

  if (!pzt::core::delete_project(*id).ok()) {
    std::fprintf(stderr, "pzt delete: 找不到项目 '%s'\n", name.c_str());
    return 1;
  }
  std::printf("已删除项目 '%s' 的元数据\n", name.c_str());
  return 0;
}

int tag_list(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt tag list: 缺少 <project_name>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag list", args[0]);
  if (!project_id) return 1;

  auto tags = pzt::core::list_tags(*project_id);
  if (tags.empty()) {
    std::printf("(还没有任何标签,用 pzt tag create 创建一个)\n");
    return 0;
  }
  for (const auto& t : tags) {
    std::printf("%-16s %6lld 张%s%s%s\n", t.name.c_str(),
                static_cast<long long>(t.tagged_count),
                t.cap ? ("  cap=" + std::to_string(*t.cap)).c_str() : "",
                t.is_ordered ? "  ordered" : "", t.is_system ? "  system" : "");
  }
  return 0;
}

int cmd_rescan(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt rescan: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  bool prune = true;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--no-prune") {
      prune = false;
    } else {
      std::fprintf(stderr, "pzt rescan: 未知参数 '%s'\n", args[i].c_str());
      print_usage();
      return 1;
    }
  }

  auto project_id = resolve_project("pzt rescan", name);
  if (!project_id) return 1;

  auto result = pzt::core::rescan_project(*project_id, prune);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt rescan: 找不到项目 '%s'\n", name.c_str());
    return 1;
  }
  std::printf("新增 %lld 张,清除 %lld 张磁盘上已消失的记录,项目现在共 %lld 张\n",
              static_cast<long long>(result.value().added_count),
              static_cast<long long>(result.value().removed_count),
              static_cast<long long>(result.value().total_count));
  return 0;
}

int cmd_export(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    std::fprintf(stderr,
                 "pzt export: 缺少 <project_name> <tag_name> <output_folder>\n");
    print_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt export", args[0]);
  if (!project_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[1]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt export: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }
  std::string output_folder = expand_home_path(args[2]);

  auto link_mode = pzt::core::LinkMode::Copy;
  for (std::size_t i = 3; i < args.size(); ++i) {
    if (args[i] == "--link") link_mode = pzt::core::LinkMode::Symlink;
  }

  auto result = pzt::core::export_tag(*tag_id, output_folder, link_mode);
  if (!result.ok()) {
    if (result.error() == pzt::core::ExportTagError::IoError) {
      std::fprintf(stderr, "pzt export: 导出目标 '%s' 无法写入(权限不足或路径被占用)\n",
                   output_folder.c_str());
    } else {
      std::fprintf(stderr, "pzt export: 找不到标签 '%s'\n", args[1].c_str());
    }
    return 1;
  }

  const auto& r = result.value();
  if (r.exported_count == 0 && r.skipped.empty()) {
    std::printf("标签 '%s' 下没有图片,未导出\n", args[1].c_str());
    return 0;
  }
  std::printf("已导出 %d 张到 '%s'", r.exported_count, output_folder.c_str());
  if (r.created_output_folder) std::printf("(目录不存在,已新建)");
  if (r.skipped.empty()) {
    std::printf("\n");
  } else {
    std::printf(",跳过 %zu 张:\n", r.skipped.size());
    for (const auto& s : r.skipped) {
      std::printf("  - %s: %s\n", s.file_name.c_str(), s.reason.c_str());
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

  std::fprintf(stderr, "pzt tag: 未知子命令 '%s'\n", verb.c_str());
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
    std::fprintf(stderr, "pzt recipe list: 不接受参数\n");
    print_recipe_usage();
    return 1;
  }
  auto presets = pzt::core::list_presets();
  if (presets.empty()) {
    std::printf("(没有任何预设)\n");
    return 0;
  }
  int i = 1;
  for (const auto& p : presets) {
    std::printf("%-3d %s\n", i++, p.name.c_str());
    auto versions = pzt::core::list_versions(p.id);
    int v = 1;
    for (const auto& ver : versions) {
      std::string name = ver.name.value_or("(未命名)");
      if (ver.deleted) {
        std::printf("      -   %-14s [已删除]\n", name.c_str());
      } else {
        std::printf("      %-3d %-14s highlights=%.1f shadows=%.1f wb_r=%.1f wb_b=%.1f\n", v++,
                     name.c_str(), ver.highlights, ver.shadows, ver.wb_shift_r, ver.wb_shift_b);
      }
    }
  }
  return 0;
}

int recipe_rename(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "pzt recipe rename: 缺少 <preset>:<version_number> <new_name>\n");
    print_recipe_usage();
    return 1;
  }
  auto address = parse_recipe_address(args[0]);
  if (!address) {
    std::fprintf(stderr, "pzt recipe rename: 无法解析 '%s',格式应为 <preset>:<version_number>\n",
                 args[0].c_str());
    return 1;
  }
  auto id = resolve_recipe_address(address->first, address->second);
  if (!id) {
    std::fprintf(stderr, "pzt recipe rename: 找不到 '%s'\n", args[0].c_str());
    return 1;
  }
  if (!pzt::core::rename_version(*id, args[1]).ok()) {
    std::fprintf(stderr, "pzt recipe rename: 操作失败\n");
    return 1;
  }
  std::printf("已重命名为 '%s'\n", args[1].c_str());
  return 0;
}

int recipe_delete(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt recipe delete: 缺少 <preset>:<version_number>\n");
    print_recipe_usage();
    return 1;
  }
  auto address = parse_recipe_address(args[0]);
  if (!address) {
    std::fprintf(stderr, "pzt recipe delete: 无法解析 '%s',格式应为 <preset>:<version_number>\n",
                 args[0].c_str());
    return 1;
  }
  auto id = resolve_recipe_address(address->first, address->second);
  if (!id) {
    std::fprintf(stderr, "pzt recipe delete: 找不到 '%s'\n", args[0].c_str());
    return 1;
  }
  if (!pzt::core::delete_version(*id).ok()) {
    std::fprintf(stderr, "pzt recipe delete: 操作失败\n");
    return 1;
  }
  std::printf("已删除 '%s'(软删除,已经应用它的图片渲染不受影响)\n", args[0].c_str());
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

  std::fprintf(stderr, "pzt recipe: 未知子命令 '%s'\n", verb.c_str());
  print_recipe_usage();
  return 1;
}

}  // namespace pzt::cli::commands
