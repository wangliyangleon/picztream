#include "cli/i18n/i18n.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pzt::cli::i18n {

Lang g_lang = Lang::zh;

void init_lang() {
  const char *pzt_lang = std::getenv("PZT_LANG");
  if (pzt_lang != nullptr) {
    if (std::strcmp(pzt_lang, "en") == 0 || std::strcmp(pzt_lang, "EN") == 0) {
      g_lang = Lang::en;
      return;
    }
    if (std::strcmp(pzt_lang, "zh") == 0 || std::strcmp(pzt_lang, "ZH") == 0) {
      g_lang = Lang::zh;
      return;
    }
  }

  const char *lang = std::getenv("LANG");
  if (lang != nullptr) {
    std::string l(lang);
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (l.find("zh") != std::string::npos) {
      g_lang = Lang::zh;
      return;
    } else if (l.find("en") != std::string::npos) {
      g_lang = Lang::en;
      return;
    }
  }

  g_lang = Lang::zh; // Default is zh
}

std::string reject_tag_label() {
  if (g_lang == Lang::zh) {
    return "废片";
  } else {
    return "Reject";
  }
}

std::string tag_display_name(const pzt::core::TagSummary &tag) {
  if (tag.is_system)
    return reject_tag_label();
  return tag.name;
}

std::string menu_item(const std::string &key, const std::string &label) {
  return key + ":[" + label + "]";
}

std::string usage_main() {
  if (g_lang == Lang::zh) {
    return "usage:\n"
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
           "  pzt recipe list\n";
  } else {
    return "usage:\n"
           "  pzt new <project_name> [folder_path]\n"
           "  pzt list\n"
           "  pzt open [project_name] [--debug]  (h/l Prev/Next image, "
           "j/k Next/Prev untagged image, space Tag image, x Toggle Reject, g "
           "Filter, "
           "r Apply/Clear/Create/Delete recipe, r v Temporarily preview "
           "original, "
           "q Quit; --debug displays internal logs in an area below the image, "
           "hidden and disabled by default)\n"
           "  pzt archive <project_name>\n"
           "  pzt delete <project_name>\n"
           "  pzt rescan <project_name> [--no-prune]  (Clears missing file "
           "records "
           "and their tags by default; pass --no-prune to skip pruning when "
           "running "
           "on temporarily unmounted storage)\n"
           "  pzt export <project_name> <tag_name> <output_folder> [--link]\n"
           "  pzt tag list <project_name>\n"
           "  pzt recipe list\n";
  }
}

std::string usage_tag() {
  return "usage:\n"
         "  pzt tag list <project_name>\n";
}

std::string usage_recipe() {
  return "usage:\n"
         "  pzt recipe list\n"
         "  pzt recipe rename <preset>:<version_number> <new_name>\n"
         "  pzt recipe delete <preset>:<version_number>\n";
}

std::string err_unknown_subcommand(const std::string &subcommand) {
  if (g_lang == Lang::zh) {
    return "pzt: 未知子命令 '" + subcommand + "'\n";
  } else {
    return "pzt: unknown subcommand '" + subcommand + "'\n";
  }
}

std::string err_project_not_found(const std::string &cmd,
                                  const std::string &project_name) {
  if (g_lang == Lang::zh) {
    return cmd + ": 找不到项目 '" + project_name +
           "',用 pzt list 查看可用项目\n";
  } else {
    return cmd + ": project '" + project_name +
           "' not found, use 'pzt list' to see available projects\n";
  }
}

std::string err_new_missing_name() {
  if (g_lang == Lang::zh) {
    return "pzt new: 缺少 <project_name>\n";
  } else {
    return "pzt new: missing <project_name>\n";
  }
}

std::string err_new_name_exists(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "pzt new: 项目名 '" + name + "' 已存在\n";
  } else {
    return "pzt new: project '" + name + "' already exists\n";
  }
}

std::string err_new_no_images(const std::string &folder_path) {
  if (g_lang == Lang::zh) {
    return "pzt new: '" + folder_path + "' 目录下没有找到任何 JPEG 文件\n";
  } else {
    return "pzt new: no JPEG files found in directory '" + folder_path + "'\n";
  }
}

std::string msg_project_created(const std::string &name,
                                const std::string &root_path,
                                long long image_count) {
  if (g_lang == Lang::zh) {
    return "已创建项目 '" + name + "'(" + root_path + "),共 " +
           std::to_string(image_count) + " 张 JPEG\n";
  } else {
    return "Project '" + name + "'(" + root_path + ") created, total " +
           std::to_string(image_count) + " JPEGs\n";
  }
}

std::string msg_project_created_simple(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "已创建项目 '" + name + "'\n";
  } else {
    return "Project '" + name + "' created\n";
  }
}

std::string err_archive_missing_name() {
  if (g_lang == Lang::zh) {
    return "pzt archive: 缺少 <project_name>\n";
  } else {
    return "pzt archive: missing <project_name>\n";
  }
}

std::string err_archive_failed(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "pzt archive: 找不到项目 '" + name + "'\n";
  } else {
    return "pzt archive: project '" + name + "' not found\n";
  }
}

std::string msg_project_archived(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "已归档项目 '" + name + "'\n";
  } else {
    return "Project '" + name + "' archived\n";
  }
}

std::string err_delete_missing_name() {
  if (g_lang == Lang::zh) {
    return "pzt delete: 缺少 <project_name>\n";
  } else {
    return "pzt delete: missing <project_name>\n";
  }
}

std::string msg_delete_warn_prompt(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "即将删除项目 '" + name +
           "' 的全部标签与浏览状态,不影响磁盘上的照片文件,此操作不可撤销。\n";
  } else {
    return "Deleting all tags and browsing states for project '" + name +
           "'. Disk photo files will not be affected. This action is "
           "irreversible.\n";
  }
}

std::string msg_delete_confirm_input() {
  if (g_lang == Lang::zh) {
    return "请再次输入项目名确认删除: ";
  } else {
    return "Enter project name again to confirm deletion: ";
  }
}

std::string msg_delete_cancelled() {
  if (g_lang == Lang::zh) {
    return "已取消,项目未被删除\n";
  } else {
    return "Cancelled, project not deleted\n";
  }
}

std::string err_delete_failed(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "pzt delete: 找不到项目 '" + name + "'\n";
  } else {
    return "pzt delete: project '" + name + "' not found\n";
  }
}

std::string msg_project_deleted(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "已删除项目 '" + name + "' 的元数据\n";
  } else {
    return "Metadata for project '" + name + "' deleted\n";
  }
}

std::string err_tag_list_missing_name() {
  if (g_lang == Lang::zh) {
    return "pzt tag list: 缺少 <project_name>\n";
  } else {
    return "pzt tag list: missing <project_name>\n";
  }
}

std::string msg_tag_list_empty() {
  if (g_lang == Lang::zh) {
    return "(还没有任何标签,用 pzt tag create 创建一个)\n";
  } else {
    return "(No tags yet. Create one with 'pzt tag create')\n";
  }
}

std::string msg_tag_item(const std::string &name, long long count,
                         std::optional<std::int64_t> cap, bool is_ordered,
                         bool is_system) {
  std::string cap_str = cap ? "  cap=" + std::to_string(*cap) : "";
  std::string ord_str = is_ordered ? "  ordered" : "";
  std::string sys_str = is_system ? "  system" : "";
  char buf[256];
  if (g_lang == Lang::zh) {
    std::snprintf(buf, sizeof(buf), "%-16s %6lld 张%s%s%s\n", name.c_str(),
                  count, cap_str.c_str(), ord_str.c_str(), sys_str.c_str());
  } else {
    std::snprintf(buf, sizeof(buf), "%-16s %6lld images%s%s%s\n", name.c_str(),
                  count, cap_str.c_str(), ord_str.c_str(), sys_str.c_str());
  }
  return buf;
}

std::string msg_project_list_empty() {
  if (g_lang == Lang::zh) {
    return "(还没有任何项目,用 pzt new 创建一个)\n";
  } else {
    return "(No projects yet. Create one with 'pzt new')\n";
  }
}

std::string msg_project_item(const std::string &name, long long image_count,
                             const std::string &root_path, bool archived) {
  char buf[512];
  if (g_lang == Lang::zh) {
    std::snprintf(buf, sizeof(buf), "%-20s %8lld 张  %s%s\n", name.c_str(),
                  image_count, root_path.c_str(), archived ? "  [已归档]" : "");
  } else {
    std::snprintf(buf, sizeof(buf), "%-20s %8lld images  %s%s\n", name.c_str(),
                  image_count, root_path.c_str(),
                  archived ? "  [Archived]" : "");
  }
  return buf;
}

std::string err_rescan_missing_name() {
  if (g_lang == Lang::zh) {
    return "pzt rescan: 缺少 <project_name>\n";
  } else {
    return "pzt rescan: missing <project_name>\n";
  }
}

std::string err_rescan_unknown_arg(const std::string &arg) {
  if (g_lang == Lang::zh) {
    return "pzt rescan: 未知参数 '" + arg + "'\n";
  } else {
    return "pzt rescan: unknown option '" + arg + "'\n";
  }
}

std::string err_rescan_failed(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "pzt rescan: 找不到项目 '" + name + "'\n";
  } else {
    return "pzt rescan: project '" + name + "' not found\n";
  }
}

std::string msg_rescan_result(long long added, long long removed,
                              long long total) {
  if (g_lang == Lang::zh) {
    return "新增 " + std::to_string(added) + " 张,清除 " +
           std::to_string(removed) + " 张磁盘上已消失的记录,项目现在共 " +
           std::to_string(total) + " 张\n";
  } else {
    return "Added " + std::to_string(added) + " images, cleared " +
           std::to_string(removed) + " missing records, project now has " +
           std::to_string(total) + " images\n";
  }
}

std::string err_export_missing_args() {
  if (g_lang == Lang::zh) {
    return "pzt export: 缺少 <project_name> <tag_name> <output_folder>\n";
  } else {
    return "pzt export: missing <project_name> <tag_name> <output_folder>\n";
  }
}

std::string err_export_tag_not_found(const std::string &tag_name) {
  if (g_lang == Lang::zh) {
    return "pzt export: 找不到标签 '" + tag_name + "'\n";
  } else {
    return "pzt export: tag '" + tag_name + "' not found\n";
  }
}

std::string err_export_io_error(const std::string &path) {
  if (g_lang == Lang::zh) {
    return "pzt export: 导出目标 '" + path +
           "' 无法写入(权限不足或路径被占用)\n";
  } else {
    return "pzt export: export target '" + path +
           "' is not writable (insufficient permissions or path occupied)\n";
  }
}

std::string msg_export_no_images(const std::string &tag_name) {
  if (g_lang == Lang::zh) {
    return "标签 '" + tag_name + "' 下没有图片,未导出\n";
  } else {
    return "No images under tag '" + tag_name + "', not exported\n";
  }
}

std::string msg_export_success(int count, const std::string &path,
                               bool created_folder) {
  if (g_lang == Lang::zh) {
    return "已导出 " + std::to_string(count) + " 张到 '" + path + "'" +
           (created_folder ? "(目录不存在,已新建)" : "");
  } else {
    return "Exported " + std::to_string(count) + " images to '" + path + "'" +
           (created_folder ? " (created directory)" : "");
  }
}

std::string msg_export_skipped(size_t count) {
  if (g_lang == Lang::zh) {
    return ",跳过 " + std::to_string(count) + " 张:\n";
  } else {
    return ", skipped " + std::to_string(count) + " images:\n";
  }
}

std::string msg_export_skipped_item(const std::string &file_name,
                                    const std::string &reason) {
  return "  - " + file_name + ": " + reason + "\n";
}

std::string err_tag_unknown_subcommand(const std::string &verb) {
  if (g_lang == Lang::zh) {
    return "pzt tag: 未知子命令 '" + verb + "'\n";
  } else {
    return "pzt tag: unknown subcommand '" + verb + "'\n";
  }
}

std::string err_recipe_list_no_args() {
  if (g_lang == Lang::zh) {
    return "pzt recipe list: 不接受参数\n";
  } else {
    return "pzt recipe list: does not accept arguments\n";
  }
}

std::string msg_recipe_list_empty() {
  if (g_lang == Lang::zh) {
    return "(没有任何预设)\n";
  } else {
    return "(No presets found)\n";
  }
}

std::string msg_recipe_preset_item(int index, const std::string &name) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%-3d %s\n", index, name.c_str());
  return buf;
}

std::string msg_recipe_version_deleted_label() {
  if (g_lang == Lang::zh) {
    return "[已删除]";
  } else {
    return "[Deleted]";
  }
}

std::string msg_recipe_version_unnamed_label() {
  if (g_lang == Lang::zh) {
    return "(未命名)";
  } else {
    return "(Unnamed)";
  }
}

std::string msg_recipe_version_item(int v, const std::string &name, double hi,
                                    double sh, double r, double b) {
  char buf[256];
  std::snprintf(
      buf, sizeof(buf),
      "      %-3d %-14s highlights=%.1f shadows=%.1f wb_r=%.1f wb_b=%.1f\n", v,
      name.c_str(), hi, sh, r, b);
  return buf;
}

std::string err_recipe_rename_missing_args() {
  if (g_lang == Lang::zh) {
    return "pzt recipe rename: 缺少 <preset>:<version_number> <new_name>\n";
  } else {
    return "pzt recipe rename: missing <preset>:<version_number> <new_name>\n";
  }
}

std::string err_recipe_rename_invalid_address(const std::string &addr) {
  if (g_lang == Lang::zh) {
    return "pzt recipe rename: 无法解析 '" + addr +
           "',格式应为 <preset>:<version_number>\n";
  } else {
    return "pzt recipe rename: failed to parse '" + addr +
           "', format should be <preset>:<version_number>\n";
  }
}

std::string err_recipe_rename_not_found(const std::string &addr) {
  if (g_lang == Lang::zh) {
    return "pzt recipe rename: 找不到 '" + addr + "'\n";
  } else {
    return "pzt recipe rename: '" + addr + "' not found\n";
  }
}

std::string err_recipe_rename_failed() {
  if (g_lang == Lang::zh) {
    return "pzt recipe rename: 操作失败\n";
  } else {
    return "pzt recipe rename: action failed\n";
  }
}

std::string msg_recipe_renamed(const std::string &new_name) {
  if (g_lang == Lang::zh) {
    return "已重命名为 '" + new_name + "'\n";
  } else {
    return "Renamed to '" + new_name + "'\n";
  }
}

std::string err_recipe_delete_missing_args() {
  if (g_lang == Lang::zh) {
    return "pzt recipe delete: 缺少 <preset>:<version_number>\n";
  } else {
    return "pzt recipe delete: missing <preset>:<version_number>\n";
  }
}

std::string err_recipe_delete_invalid_address(const std::string &addr) {
  if (g_lang == Lang::zh) {
    return "pzt recipe delete: 无法解析 '" + addr +
           "',格式应为 <preset>:<version_number>\n";
  } else {
    return "pzt recipe delete: failed to parse '" + addr +
           "', format should be <preset>:<version_number>\n";
  }
}

std::string err_recipe_delete_not_found(const std::string &addr) {
  if (g_lang == Lang::zh) {
    return "pzt recipe delete: 找不到 '" + addr + "'\n";
  } else {
    return "pzt recipe delete: '" + addr + "' not found\n";
  }
}

std::string err_recipe_delete_failed() {
  if (g_lang == Lang::zh) {
    return "pzt recipe delete: 操作失败\n";
  } else {
    return "pzt recipe delete: action failed\n";
  }
}

std::string msg_recipe_deleted(const std::string &addr) {
  if (g_lang == Lang::zh) {
    return "已删除 '" + addr + "'(软删除,已经应用它的图片渲染不受影响)\n";
  } else {
    return "Deleted '" + addr +
           "' (soft deleted, existing rendering is unaffected)\n";
  }
}

std::string err_recipe_unknown_subcommand(const std::string &verb) {
  if (g_lang == Lang::zh) {
    return "pzt recipe: 未知子命令 '" + verb + "'\n";
  } else {
    return "pzt recipe: unknown subcommand '" + verb + "'\n";
  }
}

std::string err_open_project_not_found() {
  if (g_lang == Lang::zh) {
    return "pzt open: 找不到项目,用 pzt list 查看可用项目及其路径\n";
  } else {
    return "pzt open: project not found, use 'pzt list' to see available "
           "projects and paths\n";
  }
}

std::string err_open_project_no_images(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "pzt open: 项目 '" + name + "' 里没有图片\n";
  } else {
    return "pzt open: project '" + name + "' contains no images\n";
  }
}

std::string err_open_tmux_passthrough() {
  if (g_lang == Lang::zh) {
    return "pzt open: 当前 Tmux 会话未开启 allow-passthrough,Kitty "
           "图形协议无法穿透到 Ghostty。请在 tmux.conf 里加 `set -g "
           "allow-passthrough on` 后重启会话,或在独立 Ghostty 窗口(不经过 "
           "Tmux)里直接运行\n";
  } else {
    return "pzt open: tmux allow-passthrough is disabled; Kitty graphics "
           "protocol cannot reach Ghostty. Please add `set -g "
           "allow-passthrough on` to your tmux.conf and restart tmux, or run "
           "directly in Ghostty (outside tmux).\n";
  }
}

std::string banner_text() {
  if (g_lang == Lang::zh) {
    return " h/l:[上一张/下一张]   j/k:[下一张/上一张未打标签]   "
           "space:[打标签]   x:[标记废片]"
           "   g:[筛选]   r:[风格]   q:[退出] ";
  } else {
    return " h/l:[Prev/Next]   j/k:[Next/Prev Untagged]   space:[Tag]   "
           "x:[Toggle Reject]"
           "   g:[Filter]   r:[Recipe]   q:[Quit] ";
  }
}

std::string info_filter_label(const std::string &tag_name) {
  if (g_lang == Lang::zh) {
    return "  筛选: " + tag_name;
  } else {
    return "  Filter: " + tag_name;
  }
}

std::string info_tags_label() {
  if (g_lang == Lang::zh) {
    return "标签:";
  } else {
    return "Tags:";
  }
}

std::string info_none_label() {
  if (g_lang == Lang::zh) {
    return "(无)";
  } else {
    return "(None)";
  }
}

std::string info_size_label(const std::string &size_str) {
  if (g_lang == Lang::zh) {
    return "大小: " + size_str;
  } else {
    return "Size: " + size_str;
  }
}

std::string info_style_label() {
  if (g_lang == Lang::zh) {
    return "风格:";
  } else {
    return "Recipe:";
  }
}

std::string info_style_none_label() {
  if (g_lang == Lang::zh) {
    return "  (无)";
  } else {
    return "  (None)";
  }
}

std::string msg_press_any_key_to_continue(const std::string &status) {
  if (g_lang == Lang::zh) {
    return status + ",按任意键继续 ";
  } else {
    return status + ", press any key to continue ";
  }
}

std::string err_open_render_failed() {
  if (g_lang == Lang::zh) {
    return "pzt open: 渲染失败\n";
  } else {
    return "pzt open: render failed\n";
  }
}

std::string err_open_decode_failed() {
  if (g_lang == Lang::zh) {
    return "pzt open: 图片解码失败,跳过\n";
  } else {
    return "pzt open: image decoding failed, skipping\n";
  }
}

std::string msg_all_tagged() {
  if (g_lang == Lang::zh) {
    return " 所有图片都已打过标签 ";
  } else {
    return " All images are already tagged ";
  }
}

std::string err_remove_tag_failed() {
  if (g_lang == Lang::zh) {
    return " 摘标签失败,请重试 ";
  } else {
    return " Failed to remove tag, please try again ";
  }
}

std::string err_filter_failed() {
  if (g_lang == Lang::zh) {
    return " 筛选失败,请重试 ";
  } else {
    return " Filtering failed, please try again ";
  }
}

std::string msg_filter_no_images() {
  if (g_lang == Lang::zh) {
    return " 该标签下暂无图片 ";
  } else {
    return " No images found under this tag ";
  }
}

std::string msg_browse_exited() {
  if (g_lang == Lang::zh) {
    return "已退出浏览\n";
  } else {
    return "Exited browse mode\n";
  }
}

std::string tag_menu_cap_zero() {
  if (g_lang == Lang::zh) {
    return " 标签上限为 0,无法添加 ";
  } else {
    return " Tag cap is 0, cannot tag ";
  }
}

std::string tag_menu_full(int cap) {
  if (g_lang == Lang::zh) {
    return " 已满(" + std::to_string(cap) + "):";
  } else {
    return " Full(" + std::to_string(cap) + "):";
  }
}

std::string tag_menu_esc_cancel() {
  if (g_lang == Lang::zh) {
    return "  " + menu_item("Esc", "取消");
  } else {
    return "  " + menu_item("Esc", "Cancel");
  }
}

std::string tag_menu_replace_failed() {
  if (g_lang == Lang::zh) {
    return " 替换失败,请重试 ";
  } else {
    return " Replace failed, please try again ";
  }
}

std::string tag_menu_replaced(const std::string &old_file) {
  if (g_lang == Lang::zh) {
    return " 已替换 '" + old_file + "' ";
  } else {
    return " Replaced '" + old_file + "' ";
  }
}

std::string tag_menu_remove_prefix() {
  if (g_lang == Lang::zh) {
    return " 摘除:" + menu_item("0", reject_tag_label());
  } else {
    return " Remove:" + menu_item("0", reject_tag_label());
  }
}

std::string tag_menu_remove_failed() {
  if (g_lang == Lang::zh) {
    return " 摘标签失败,请重试 ";
  } else {
    return " Remove tag failed, please try again ";
  }
}

std::string tag_menu_new_name_prompt() {
  if (g_lang == Lang::zh) {
    return " 新标签名称: ";
  } else {
    return " New tag name: ";
  }
}

std::string tag_menu_new_name_empty() {
  if (g_lang == Lang::zh) {
    return " 标签名不能为空,已取消 ";
  } else {
    return " Tag name cannot be empty, cancelled ";
  }
}

std::string tag_menu_cap_prompt() {
  if (g_lang == Lang::zh) {
    return " 上限数量(直接 Enter = 不限): ";
  } else {
    return " Capacity cap (Enter = unlimited): ";
  }
}

std::string tag_menu_order_prompt() {
  if (g_lang == Lang::zh) {
    return " 是否需要按顺序排列(用于朋友圈九宫格等,直接 Enter = 否): ";
  } else {
    return " Ordered tag (Enter = No): ";
  }
}

std::string tag_menu_ordered_keys_help() {
  if (g_lang == Lang::zh) {
    return menu_item("y", "是") + " / " + menu_item("其它键", "否") + " ";
  } else {
    return menu_item("y", "Yes") + " / " + menu_item("other keys", "No") + " ";
  }
}

std::string tag_menu_name_exists(const std::string &name) {
  if (g_lang == Lang::zh) {
    return " 标签名 '" + name + "' 已存在,未创建 ";
  } else {
    return " Tag '" + name + "' already exists, not created ";
  }
}

std::string tag_menu_created(const std::string &name) {
  if (g_lang == Lang::zh) {
    return " 已创建标签 '" + name + "' ";
  } else {
    return " Created tag '" + name + "' ";
  }
}

std::string tag_menu_no_deletable() {
  if (g_lang == Lang::zh) {
    return " 没有可删除的标签 ";
  } else {
    return " No deletable tags ";
  }
}

std::string tag_menu_delete_prefix() {
  if (g_lang == Lang::zh) {
    return " 删除:";
  } else {
    return " Delete:";
  }
}

std::string tag_menu_delete_item(int index, const std::string &name,
                                 long long tagged_count) {
  if (g_lang == Lang::zh) {
    return menu_item(std::to_string(index), name) + "(" +
           std::to_string(tagged_count) + "张)";
  } else {
    return menu_item(std::to_string(index), name) + "(" +
           std::to_string(tagged_count) + ")";
  }
}

std::string tag_menu_delete_confirm(const std::string &name, long long count) {
  if (g_lang == Lang::zh) {
    return " 确定删除标签 '" + name + "'(" + std::to_string(count) +
           " 张关联)?此操作不可撤销。" + menu_item("y", "确认") + " / " +
           menu_item("其它键", "取消") + " ";
  } else {
    return " Delete tag '" + name + "' (" + std::to_string(count) +
           " associated)? Irreversible. " + menu_item("y", "Confirm") + " / " +
           menu_item("other keys", "Cancel") + " ";
  }
}

std::string tag_menu_deleted(const std::string &name) {
  if (g_lang == Lang::zh) {
    return " 已删除标签 '" + name + "' ";
  } else {
    return " Deleted tag '" + name + "' ";
  }
}

std::string tag_menu_delete_failed() {
  if (g_lang == Lang::zh) {
    return " 删除失败,请重试 ";
  } else {
    return " Delete failed, please try again ";
  }
}

std::string tag_menu_add_failed() {
  if (g_lang == Lang::zh) {
    return " 打标签失败,请重试 ";
  } else {
    return " Tagging failed, please try again ";
  }
}

std::string
tag_menu_main_prompt(const std::vector<pzt::core::TagSummary> &tags) {
  if (g_lang == Lang::zh) {
    std::string line = " " + menu_item("0", reject_tag_label());
    for (size_t i = 0; i < tags.size(); ++i) {
      line += "  " + menu_item(std::to_string(i + 1), tags[i].name);
      if (tags[i].cap) {
        line += "(" + std::to_string(tags[i].tagged_count) + "/" +
                std::to_string(*tags[i].cap) + ")";
      }
    }
    line += "  " + menu_item("c", "新建") + "  " + menu_item("d", "删除") +
            "  " + menu_item("-", "摘除") + "  " + menu_item("Esc", "取消");
    return line;
  } else {
    std::string line = " " + menu_item("0", reject_tag_label());
    for (size_t i = 0; i < tags.size(); ++i) {
      line += "  " + menu_item(std::to_string(i + 1), tags[i].name);
      if (tags[i].cap) {
        line += "(" + std::to_string(tags[i].tagged_count) + "/" +
                std::to_string(*tags[i].cap) + ")";
      }
    }
    line += "  " + menu_item("c", "New") + "  " + menu_item("d", "Delete") +
            "  " + menu_item("-", "Remove") + "  " + menu_item("Esc", "Cancel");
    return line;
  }
}

std::string filter_menu_export_prefix() {
  if (g_lang == Lang::zh) {
    return " 导出:";
  } else {
    return " Export:";
  }
}

std::string filter_menu_export_current(const std::string &name) {
  if (g_lang == Lang::zh) {
    return menu_item("e", "当前筛选") + "(" + name + ")  ";
  } else {
    return menu_item("e", "Current filter") + "(" + name + ")  ";
  }
}

std::string filter_menu_export_to_prompt() {
  if (g_lang == Lang::zh) {
    return " 导出到: ";
  } else {
    return " Export to: ";
  }
}

std::string filter_menu_export_path_empty() {
  if (g_lang == Lang::zh) {
    return " 导出路径不能为空,已取消 ";
  } else {
    return " Export path cannot be empty, cancelled ";
  }
}

std::string filter_menu_export_io_error(const std::string &path) {
  if (g_lang == Lang::zh) {
    return " 导出目标 '" + path + "' 无法写入(权限不足或路径被占用) ";
  } else {
    return " Export path '" + path +
           "' not writable (insufficient permissions or path occupied) ";
  }
}

std::string filter_menu_export_failed() {
  if (g_lang == Lang::zh) {
    return " 导出失败,请重试 ";
  } else {
    return " Export failed, please try again ";
  }
}

std::string filter_menu_export_no_images(const std::string &name) {
  if (g_lang == Lang::zh) {
    return " 标签 '" + name + "' 下没有图片,未导出 ";
  } else {
    return " No images found under tag '" + name + "', not exported ";
  }
}

std::string filter_menu_export_success(int count, const std::string &name,
                                       const std::string &path,
                                       bool created_folder,
                                       size_t skipped_count) {
  if (g_lang == Lang::zh) {
    std::string status = " 已导出 " + std::to_string(count) + " 张 '" + name +
                         "' 到 '" + path + "'";
    if (created_folder)
      status += "(目录不存在,已新建)";
    if (skipped_count > 0)
      status += "(跳过 " + std::to_string(skipped_count) + " 张)";
    status += " ";
    return status;
  } else {
    std::string status = " Exported " + std::to_string(count) + " of '" + name +
                         "' to '" + path + "'";
    if (created_folder)
      status += " (created directory)";
    if (skipped_count > 0)
      status += " (skipped " + std::to_string(skipped_count) + ")";
    status += " ";
    return status;
  }
}

std::string
filter_menu_main_prompt(const std::vector<pzt::core::TagSummary> &tags) {
  if (g_lang == Lang::zh) {
    std::string line = " " + menu_item("g", "清除筛选") + "  " +
                       menu_item("e", "导出") + "  " +
                       menu_item("0", reject_tag_label());
    for (size_t i = 0; i < tags.size(); ++i) {
      line += "  " + menu_item(std::to_string(i + 1), tags[i].name);
    }
    line += "  " + menu_item("Esc", "取消");
    return line;
  } else {
    std::string line = " " + menu_item("g", "Clear Filter") + "  " +
                       menu_item("e", "Export") + "  " +
                       menu_item("0", reject_tag_label());
    for (size_t i = 0; i < tags.size(); ++i) {
      line += "  " + menu_item(std::to_string(i + 1), tags[i].name);
    }
    line += "  " + menu_item("Esc", "Cancel");
    return line;
  }
}

std::string recipe_menu_select_preset_prefix() {
  if (g_lang == Lang::zh) {
    return " 选预设:";
  } else {
    return " Select preset:";
  }
}

std::string recipe_menu_preset_not_exist() {
  if (g_lang == Lang::zh) {
    return " 预设不存在 ";
  } else {
    return " Preset not found ";
  }
}

std::string recipe_menu_version_prompt(const std::string &preset_name) {
  if (g_lang == Lang::zh) {
    return " " + preset_name + ":  " + menu_item("0", "默认");
  } else {
    return " " + preset_name + ":  " + menu_item("0", "Default");
  }
}

std::string recipe_menu_version_default_label() {
  if (g_lang == Lang::zh) {
    return "(未命名)";
  } else {
    return "(Unnamed)";
  }
}

std::string recipe_menu_no_deletable_versions(const std::string &preset_name) {
  if (g_lang == Lang::zh) {
    return " '" + preset_name + "' 下没有可删除的 version ";
  } else {
    return " No deletable versions under '" + preset_name + "' ";
  }
}

std::string recipe_menu_delete_version_prefix(const std::string &preset_name) {
  if (g_lang == Lang::zh) {
    return " 删除(" + preset_name + "):";
  } else {
    return " Delete(" + preset_name + "):";
  }
}

std::string recipe_menu_delete_failed() {
  if (g_lang == Lang::zh) {
    return " 删除失败,请重试 ";
  } else {
    return " Delete failed, please try again ";
  }
}

std::string recipe_menu_delete_success(const std::string &name) {
  if (g_lang == Lang::zh) {
    return " 已删除 '" + name + "' ";
  } else {
    return " Deleted '" + name + "' ";
  }
}

std::string recipe_menu_custom_full(const std::string &preset_name) {
  if (g_lang == Lang::zh) {
    return " '" + preset_name +
           "' 下自定义配方已满(最多 9 个),先删除一些再新建 ";
  } else {
    return " Custom recipe list is full under '" + preset_name +
           "' (max 9), delete some first ";
  }
}

std::string recipe_menu_input_highlights() {
  if (g_lang == Lang::zh) {
    return " 高光(直接 Enter = 0): ";
  } else {
    return " Highlights (Enter = 0): ";
  }
}

std::string recipe_menu_input_shadows() {
  if (g_lang == Lang::zh) {
    return " 暗光(直接 Enter = 0): ";
  } else {
    return " Shadows (Enter = 0): ";
  }
}

std::string recipe_menu_input_wb_r() {
  if (g_lang == Lang::zh) {
    return " 白平衡-红(直接 Enter = 0): ";
  } else {
    return " WhiteBalance-Red (Enter = 0): ";
  }
}

std::string recipe_menu_input_wb_b() {
  if (g_lang == Lang::zh) {
    return " 白平衡-蓝(直接 Enter = 0): ";
  } else {
    return " WhiteBalance-Blue (Enter = 0): ";
  }
}

std::string recipe_menu_input_name() {
  if (g_lang == Lang::zh) {
    return " 名称(可选,直接 Enter = 不设置): ";
  } else {
    return " Name (Optional, Enter = None): ";
  }
}

std::string recipe_menu_create_failed() {
  if (g_lang == Lang::zh) {
    return " 创建失败,请重试 ";
  } else {
    return " Create failed, please try again ";
  }
}

std::string recipe_menu_create_success(const std::string &preset_name) {
  if (g_lang == Lang::zh) {
    return " 已在 '" + preset_name + "' 下创建新 version ";
  } else {
    return " Created new version under '" + preset_name + "' ";
  }
}

std::string
recipe_menu_main_prompt(bool has_recipe,
                        const std::vector<pzt::core::PresetSummary> &presets) {
  if (g_lang == Lang::zh) {
    std::string line = " " + menu_item("r", "清除");
    if (has_recipe)
      line += "  " + menu_item("v", "切换原图/风格化");
    line += "  " + menu_item("c", "新建") + "  " + menu_item("d", "删除");
    for (size_t i = 0; i < presets.size(); ++i) {
      line += "  " + menu_item(std::to_string(i + 1), presets[i].name);
    }
    line += "  " + menu_item("Esc", "取消");
    return line;
  } else {
    std::string line = " " + menu_item("r", "Clear");
    if (has_recipe)
      line += "  " + menu_item("v", "Toggle Original/Style");
    line += "  " + menu_item("c", "New") + "  " + menu_item("d", "Delete");
    for (size_t i = 0; i < presets.size(); ++i) {
      line += "  " + menu_item(std::to_string(i + 1), presets[i].name);
    }
    line += "  " + menu_item("Esc", "Cancel");
    return line;
  }
}

std::string recipe_menu_clear_failed() {
  if (g_lang == Lang::zh) {
    return " 清除失败,请重试 ";
  } else {
    return " Clear failed, please try again ";
  }
}

std::string recipe_menu_apply_failed() {
  if (g_lang == Lang::zh) {
    return " 应用失败,请重试 ";
  } else {
    return " Apply failed, please try again ";
  }
}

std::string recipe_menu_invalid_key() {
  if (g_lang == Lang::zh) {
    return " 无效按键 ";
  } else {
    return " Invalid key ";
  }
}

} // namespace pzt::cli::i18n
