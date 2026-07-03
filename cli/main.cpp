#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "core/api.h"

namespace {

void print_usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  pzt new <project_name> [folder_path]\n"
               "  pzt list\n"
               "  pzt open [project_name]\n"
               "  pzt archive <project_name>\n"
               "  pzt delete <project_name>\n"
               "  pzt rescan <project_name>\n"
               "  pzt export <project_name> <tag_name> <output_folder> [--link]\n"
               "  pzt tag create|list|add|remove|replace ...  "
               "(临时调试命令,increment 6 会被全键盘交互替换,pzt tag 查看用法)\n"
               "  pzt browse next|prev|next-untagged|prev-untagged|filter ...  "
               "(同上,pzt browse 查看用法)\n");
}

void print_browse_usage() {
  std::fprintf(stderr,
               "usage (临时调试命令,后续会被全键盘交互替换):\n"
               "  pzt browse next <project_name> [current_image_relative_path]\n"
               "  pzt browse prev <project_name> [current_image_relative_path]\n"
               "  pzt browse next-untagged <project_name> [current_image_relative_path]\n"
               "  pzt browse prev-untagged <project_name> [current_image_relative_path]\n"
               "  pzt browse filter <project_name> <tag_name>\n");
}

void print_tag_usage() {
  std::fprintf(stderr,
               "usage (临时调试命令,后续会被全键盘交互替换):\n"
               "  pzt tag create <project_name> <tag_name> [--cap N] [--ordered]\n"
               "  pzt tag list <project_name>\n"
               "  pzt tag add <project_name> <image_relative_path> <tag_name>\n"
               "  pzt tag remove <project_name> <image_relative_path> <tag_name>\n"
               "  pzt tag replace <project_name> <tag_name> <old_image_relative_path> "
               "<new_image_relative_path>\n");
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

std::optional<pzt::core::ImageId> resolve_image(const std::string& cmd,
                                                 pzt::core::ProjectId project_id,
                                                 const std::string& relative_path) {
  auto id = pzt::core::find_image_by_path(project_id, relative_path);
  if (!id) {
    std::fprintf(stderr, "%s: 项目内找不到文件 '%s'\n", cmd.c_str(), relative_path.c_str());
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

int cmd_open(const std::vector<std::string>& args) {
  std::optional<pzt::core::ProjectId> id =
      args.empty() ? pzt::core::find_project_by_root_path(std::filesystem::current_path().string())
                   : pzt::core::find_project_by_name(args[0]);
  if (!id) {
    std::fprintf(stderr, "pzt open: 找不到项目,用 pzt list 查看可用项目及其路径\n");
    return 1;
  }

  auto result = pzt::core::open_project(*id);
  if (!result.ok()) {
    // id came from a lookup that just succeeded, so this shouldn't normally
    // happen - handle it rather than assume it can't.
    std::fprintf(stderr, "pzt open: 找不到项目,用 pzt list 查看可用项目及其路径\n");
    return 1;
  }

  const auto& p = result.value();
  std::printf("已打开项目 '%s'(%s),共 %lld 张 JPEG%s\n", p.name.c_str(), p.root_path.c_str(),
              static_cast<long long>(p.image_count), p.archived ? "  [已归档]" : "");
  std::printf("(全键盘浏览界面还没实现,这是项目生命周期 increment 的桩)\n");
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

int tag_create(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "pzt tag create: 缺少 <project_name> <tag_name>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag create", args[0]);
  if (!project_id) return 1;
  const std::string& tag_name = args[1];

  std::optional<std::int64_t> cap;
  bool is_ordered = false;
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--cap" && i + 1 < args.size()) {
      cap = std::atoll(args[++i].c_str());
    } else if (args[i] == "--ordered") {
      is_ordered = true;
    }
  }

  auto result = pzt::core::create_tag(*project_id, tag_name, cap, is_ordered);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt tag create: 标签 '%s' 已存在\n", tag_name.c_str());
    return 1;
  }
  std::printf("已创建标签 '%s'%s%s\n", tag_name.c_str(),
              cap ? (" cap=" + std::to_string(*cap)).c_str() : "",
              is_ordered ? " ordered" : "");
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

void print_cap_exceeded(const pzt::core::CapExceededInfo& info) {
  std::fprintf(stderr, "标签已满(上限 %lld 张),现有条目:\n", static_cast<long long>(info.cap));
  int i = 1;
  for (const auto& entry : info.existing_entries) {
    std::fprintf(stderr, "  %d. %s\n", i++, entry.file_name.c_str());
  }
  std::fprintf(stderr, "用 pzt tag replace 指定要替换的文件\n");
}

int tag_add(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    std::fprintf(stderr, "pzt tag add: 缺少 <project_name> <image_relative_path> <tag_name>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag add", args[0]);
  if (!project_id) return 1;
  auto image_id = resolve_image("pzt tag add", *project_id, args[1]);
  if (!image_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[2]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt tag add: 找不到标签 '%s'\n", args[2].c_str());
    return 1;
  }

  auto result = pzt::core::add_tag(*image_id, *tag_id);
  if (!result.ok()) {
    const auto& err = result.error();
    switch (err.kind) {
      case pzt::core::AddTagFailureKind::TagNotFound:
        std::fprintf(stderr, "pzt tag add: 找不到标签\n");
        break;
      case pzt::core::AddTagFailureKind::ImageNotFound:
        std::fprintf(stderr, "pzt tag add: 找不到图片\n");
        break;
      case pzt::core::AddTagFailureKind::ProjectMismatch:
        std::fprintf(stderr, "pzt tag add: 图片和标签不属于同一个项目\n");
        break;
      case pzt::core::AddTagFailureKind::CapExceeded:
        print_cap_exceeded(*err.cap_info);
        break;
    }
    return 1;
  }
  std::printf("已给 '%s' 打上标签 '%s'\n", args[1].c_str(), args[2].c_str());
  return 0;
}

int tag_remove(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    std::fprintf(stderr, "pzt tag remove: 缺少 <project_name> <image_relative_path> <tag_name>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag remove", args[0]);
  if (!project_id) return 1;
  auto image_id = resolve_image("pzt tag remove", *project_id, args[1]);
  if (!image_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[2]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt tag remove: 找不到标签 '%s'\n", args[2].c_str());
    return 1;
  }

  if (!pzt::core::remove_tag(*image_id, *tag_id).ok()) {
    std::fprintf(stderr, "pzt tag remove: 找不到标签或图片\n");
    return 1;
  }
  std::printf("已移除 '%s' 的标签 '%s'\n", args[1].c_str(), args[2].c_str());
  return 0;
}

int tag_replace(const std::vector<std::string>& args) {
  if (args.size() < 4) {
    std::fprintf(stderr,
                 "pzt tag replace: 缺少 <project_name> <tag_name> "
                 "<old_image_relative_path> <new_image_relative_path>\n");
    print_tag_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt tag replace", args[0]);
  if (!project_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[1]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt tag replace: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }
  auto old_image_id = resolve_image("pzt tag replace", *project_id, args[2]);
  if (!old_image_id) return 1;
  auto new_image_id = resolve_image("pzt tag replace", *project_id, args[3]);
  if (!new_image_id) return 1;

  auto result = pzt::core::replace_tag_entry(*tag_id, *old_image_id, *new_image_id);
  if (!result.ok()) {
    switch (result.error()) {
      case pzt::core::ReplaceTagError::TagNotFound:
        std::fprintf(stderr, "pzt tag replace: 找不到标签\n");
        break;
      case pzt::core::ReplaceTagError::OldImageNotTagged:
        std::fprintf(stderr, "pzt tag replace: '%s' 目前没有这个标签\n", args[2].c_str());
        break;
      case pzt::core::ReplaceTagError::NewImageNotFound:
        std::fprintf(stderr, "pzt tag replace: 找不到新图片 '%s'\n", args[3].c_str());
        break;
    }
    return 1;
  }
  std::printf("已用 '%s' 替换 '%s' 在标签 '%s' 下的位置\n", args[3].c_str(), args[2].c_str(),
              args[1].c_str());
  return 0;
}

int cmd_rescan(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt rescan: missing <project_name>\n");
    print_usage();
    return 1;
  }
  const std::string& name = args[0];
  auto project_id = resolve_project("pzt rescan", name);
  if (!project_id) return 1;

  auto result = pzt::core::rescan_project(*project_id);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt rescan: 找不到项目 '%s'\n", name.c_str());
    return 1;
  }
  std::printf("新增 %lld 张,项目现在共 %lld 张\n",
              static_cast<long long>(result.value().added_count),
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
  const std::string& output_folder = args[2];

  auto link_mode = pzt::core::LinkMode::Copy;
  for (std::size_t i = 3; i < args.size(); ++i) {
    if (args[i] == "--link") link_mode = pzt::core::LinkMode::Symlink;
  }

  auto result = pzt::core::export_tag(*tag_id, output_folder, link_mode);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt export: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }

  const auto& r = result.value();
  std::printf("已导出 %d 张到 '%s'", r.exported_count, output_folder.c_str());
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

// current_image_relative_path 参数省略时返回 nullopt (next/prev/next-
// untagged/prev-untagged 各自对"没有当前图片"有明确定义的行为)。
std::optional<pzt::core::ImageId> resolve_optional_current(pzt::core::ProjectId project_id,
                                                            const std::vector<std::string>& args,
                                                            std::size_t index) {
  if (args.size() <= index) return std::nullopt;
  return pzt::core::find_image_by_path(project_id, args[index]);
}

const pzt::core::ImageRef* find_ref(const std::vector<pzt::core::ImageRef>& images,
                                     pzt::core::ImageId id) {
  for (const auto& r : images) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

void print_nav_result(const std::vector<pzt::core::ImageRef>& images,
                      std::optional<pzt::core::ImageId> result, const char* none_message) {
  if (images.empty()) {
    std::printf("项目内没有图片\n");
    return;
  }
  if (!result) {
    std::printf("%s\n", none_message);
    return;
  }
  const auto* ref = find_ref(images, *result);
  std::printf("%s\n", ref ? ref->file_path.c_str() : "(内部错误:找不到结果图片)");
}

int browse_next(const std::vector<std::string>& args, bool prev) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt browse %s: 缺少 <project_name>\n", prev ? "prev" : "next");
    print_browse_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt browse", args[0]);
  if (!project_id) return 1;

  auto images = pzt::core::list_images(*project_id);
  auto current = resolve_optional_current(*project_id, args, 1);
  auto result = prev ? pzt::core::prev_image(images, current) : pzt::core::next_image(images, current);
  print_nav_result(images, result, "");
  return 0;
}

int browse_untagged(const std::vector<std::string>& args, bool prev) {
  if (args.empty()) {
    std::fprintf(stderr, "pzt browse %s-untagged: 缺少 <project_name>\n", prev ? "prev" : "next");
    print_browse_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt browse", args[0]);
  if (!project_id) return 1;

  auto images = pzt::core::list_images(*project_id);
  auto current = resolve_optional_current(*project_id, args, 1);
  auto result = prev ? pzt::core::prev_untagged(images, current)
                      : pzt::core::next_untagged(images, current);
  print_nav_result(images, result, "没有更多未打标签的图片了");
  return 0;
}

int browse_filter(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "pzt browse filter: 缺少 <project_name> <tag_name>\n");
    print_browse_usage();
    return 1;
  }
  auto project_id = resolve_project("pzt browse filter", args[0]);
  if (!project_id) return 1;
  auto tag_id = pzt::core::find_tag_by_name(*project_id, args[1]);
  if (!tag_id) {
    std::fprintf(stderr, "pzt browse filter: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }

  auto result = pzt::core::filter_by_tag(*tag_id);
  if (!result.ok()) {
    std::fprintf(stderr, "pzt browse filter: 找不到标签 '%s'\n", args[1].c_str());
    return 1;
  }
  if (result.value().empty()) {
    std::printf("(这个标签下还没有图片)\n");
    return 0;
  }
  for (const auto& ref : result.value()) {
    std::printf("%s\n", ref.file_path.c_str());
  }
  return 0;
}

int cmd_browse(const std::vector<std::string>& args) {
  if (args.empty()) {
    print_browse_usage();
    return 1;
  }
  const std::string& verb = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  if (verb == "next") return browse_next(rest, false);
  if (verb == "prev") return browse_next(rest, true);
  if (verb == "next-untagged") return browse_untagged(rest, false);
  if (verb == "prev-untagged") return browse_untagged(rest, true);
  if (verb == "filter") return browse_filter(rest);

  std::fprintf(stderr, "pzt browse: 未知子命令 '%s'\n", verb.c_str());
  print_browse_usage();
  return 1;
}

int cmd_tag(const std::vector<std::string>& args) {
  if (args.empty()) {
    print_tag_usage();
    return 1;
  }
  const std::string& verb = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  if (verb == "create") return tag_create(rest);
  if (verb == "list") return tag_list(rest);
  if (verb == "add") return tag_add(rest);
  if (verb == "remove") return tag_remove(rest);
  if (verb == "replace") return tag_replace(rest);

  std::fprintf(stderr, "pzt tag: 未知子命令 '%s'\n", verb.c_str());
  print_tag_usage();
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }

  std::string subcommand = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);

  if (subcommand == "new") return cmd_new(args);
  if (subcommand == "list") return cmd_list(args);
  if (subcommand == "open") return cmd_open(args);
  if (subcommand == "archive") return cmd_archive(args);
  if (subcommand == "delete") return cmd_delete(args);
  if (subcommand == "rescan") return cmd_rescan(args);
  if (subcommand == "export") return cmd_export(args);
  if (subcommand == "tag") return cmd_tag(args);
  if (subcommand == "browse") return cmd_browse(args);

  std::fprintf(stderr, "pzt: 未知子命令 '%s'\n", subcommand.c_str());
  print_usage();
  return 1;
}
