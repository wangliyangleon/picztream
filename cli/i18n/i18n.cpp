#include "cli/i18n/i18n.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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

  // F-12：config.json 的 lang 字段——比系统 LANG 更明确的信号(用户特意为
  // PZT 配置过),但仍然可以被上面的 PZT_LANG 环境变量临时覆盖。nullopt
  // 表示配置文件里没写这个字段,继续往下走系统 LANG 检测,不当成"用户
  // 配置成了某个值"。
  auto configured_lang = pzt::core::load_settings().lang;
  if (configured_lang == "en") {
    g_lang = Lang::en;
    return;
  }
  if (configured_lang == "zh") {
    g_lang = Lang::zh;
    return;
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

std::string duplicate_tag_label() {
  if (g_lang == Lang::zh) {
    return "重复";
  } else {
    return "Duplicate";
  }
}

// M3 之前只有"废片"一个系统标签，`is_system` 就足够当判断依据；`重复`
// 标签(core::dedup)加进来之后，`is_system` 不再能唯一确定是哪个系统标
// 签——两个都是 is_system=1，这里必须再按 tag.name 精确匹配区分，不然
// 所有系统标签在界面上都会被误显示成"废片"(这就是实际发生过的 bug：
// 重复检测正确把图片打上了"重复"标签，界面却把它显示成"废片"，数据库
// 里的数据是对的，只是这个函数认错了)。
std::string tag_display_name(const pzt::core::TagSummary &tag) {
  if (tag.is_system) {
    if (tag.name == pzt::core::tagging::kDuplicateTagName) return duplicate_tag_label();
    return reject_tag_label();
  }
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
           "j/k 下一张/上一张未打标签,space 打标签,x 标记废片,e 导出当前图片,g 筛选,"
           "r 应用/清除/新建/删除风格,r v 临时预览原图,"
           "q 退出;--debug 时在图片下方开一块区域滚动显示内部日志,默认"
           "不显示也不产生这些日志)\n"
           "  pzt archive <project_name>\n"
           "  pzt delete <project_name>\n"
           "  pzt rescan <project_name> [--no-prune]  (默认会清除磁盘上已消失的"
           "文件记录,连带清掉其标签;对着可能暂时没挂载完整的存储位置跑时,"
           "加 --no-prune 跳过清理)\n"
           "  pzt export <project_name> <tag_name> <output_folder>\n"
           "  pzt tag list <project_name>\n"
           "  pzt recipe list\n";
  } else {
    return "usage:\n"
           "  pzt new <project_name> [folder_path]\n"
           "  pzt list\n"
           "  pzt open [project_name] [--debug]  (h/l Prev/Next image, "
           "j/k Next/Prev untagged image, space Tag image, x Toggle Reject, "
           "e Export current image, g Filter, "
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
           "  pzt export <project_name> <tag_name> <output_folder>\n"
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

std::string err_internal_error(const std::string &what) {
  if (g_lang == Lang::zh) {
    return "pzt: 内部错误: " + what + "\n";
  } else {
    return "pzt: internal error: " + what + "\n";
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
    return "pzt new: '" + folder_path + "' 目录下没有找到任何 JPEG/RAW 文件\n";
  } else {
    return "pzt new: no JPEG/RAW files found in directory '" + folder_path + "'\n";
  }
}

std::string err_new_unknown_arg(const std::string &arg) {
  if (g_lang == Lang::zh) {
    return "pzt new: 未知参数 '" + arg + "'\n";
  } else {
    return "pzt new: unknown option '" + arg + "'\n";
  }
}

std::string msg_raw_preview_progress(int done, int total) {
  if (g_lang == Lang::zh) {
    return "正在生成 RAW 预览缓存 (" + std::to_string(done) + "/" + std::to_string(total) + ")...";
  } else {
    return "Generating RAW preview cache (" + std::to_string(done) + "/" + std::to_string(total) +
           ")...";
  }
}

std::string msg_export_raw_progress(int done, int total) {
  if (g_lang == Lang::zh) {
    return "正在处理 RAW 图片 (" + std::to_string(done) + "/" + std::to_string(total) + ")...";
  } else {
    return "Processing RAW images (" + std::to_string(done) + "/" + std::to_string(total) + ")...";
  }
}

std::string msg_project_created(const std::string &name,
                                const std::string &root_path,
                                long long image_count) {
  if (g_lang == Lang::zh) {
    return "已创建项目 '" + name + "'(" + root_path + "),共 " +
           std::to_string(image_count) + " 张图片\n";
  } else {
    return "Project '" + name + "'(" + root_path + ") created, total " +
           std::to_string(image_count) + " images\n";
  }
}

std::string msg_project_created_simple(const std::string &name) {
  if (g_lang == Lang::zh) {
    return "已创建项目 '" + name + "'\n";
  } else {
    return "Project '" + name + "' created\n";
  }
}

std::string msg_new_press_any_key_to_open() {
  if (g_lang == Lang::zh) {
    return "按任意键打开项目...\n";
  } else {
    return "Press any key to open the project...\n";
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
                              long long total, long long upgraded) {
  std::string upgraded_suffix;
  if (upgraded > 0) {
    upgraded_suffix = g_lang == Lang::zh
                        ? ("，其中 " + std::to_string(upgraded) + " 张补上了同名 RAW,已切换成 RAW")
                        : (", " + std::to_string(upgraded) + " of which gained a matching RAW file and switched to RAW");
  }
  if (g_lang == Lang::zh) {
    return "新增 " + std::to_string(added) + " 张,清除 " +
           std::to_string(removed) + " 张磁盘上已消失的记录,项目现在共 " +
           std::to_string(total) + " 张" + upgraded_suffix + "\n";
  } else {
    return "Added " + std::to_string(added) + " images, cleared " +
           std::to_string(removed) + " missing records, project now has " +
           std::to_string(total) + " images" + upgraded_suffix + "\n";
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

std::string export_skip_reason(pzt::core::SkipReason reason) {
  switch (reason) {
    case pzt::core::SkipReason::SourceMissing:
      return g_lang == Lang::zh ? "源文件缺失" : "source file missing";
    case pzt::core::SkipReason::DecodeFailed:
      return g_lang == Lang::zh ? "解码失败" : "decode failed";
    case pzt::core::SkipReason::RenderFailed:
      return g_lang == Lang::zh ? "应用风格失败" : "apply recipe failed";
    case pzt::core::SkipReason::EncodeFailed:
      return g_lang == Lang::zh ? "编码失败" : "encode failed";
    case pzt::core::SkipReason::RawDecodeFailed:
      return g_lang == Lang::zh ? "RAW 解码失败" : "RAW decode failed";
  }
  return "";  // 不可达,四个枚举值都已覆盖;写这行只是为了不同编译器对"是
              // 否证明了 switch 穷尽"的判断不完全一致,避免 -Wreturn-type
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

// h/l、j/k、q 不在这里——它们是不会派生二级菜单的一次性动作(导航/退
// 出),挪到底部导航栏常驻显示(见 nav_bar_text),右侧这个 block 只保留
// "按下去会打开二级菜单"的那几个键,逻辑上更一致。
std::vector<MenuLine> menu_lines() {
  if (g_lang == Lang::zh) {
    return {
        {' ', menu_item("space", "打标签")},
        {'x', menu_item("x", "标记废片")},
        {0, ""},
        {'g', menu_item("g", "筛选")},
        {'e', menu_item("e", "导出")},
        {0, ""},
        {'r', menu_item("r", "风格")},
        {0, ""},
        {':', menu_item(":", "控制台")},
    };
  } else {
    return {
        {' ', menu_item("space", "Tag")},
        {'x', menu_item("x", "Toggle Reject")},
        {0, ""},
        {'g', menu_item("g", "Filter")},
        {'e', menu_item("e", "Export")},
        {0, ""},
        {'r', menu_item("r", "Recipe")},
        {0, ""},
        {':', menu_item(":", "Console")},
    };
  }
}

// 底部导航栏空闲时的常驻内容,分两行:h/l、j/k 这两组不会派生二级菜单的
// 导航键放第一行,q 退出单独占第二行——不然第二行一直空着不好看,跟右侧
// menu_lines()(都是会派生二级菜单的键)分开,两块各自逻辑一致。
std::string nav_bar_line1() {
  if (g_lang == Lang::zh) {
    return " " + menu_item("h/l", "上一张/下一张") + "   " +
           menu_item("j/k", "下一张/上一张未打标签") + " ";
  } else {
    return " " + menu_item("h/l", "Prev/Next") + "   " +
           menu_item("j/k", "Next/Prev Untagged") + " ";
  }
}

std::string nav_bar_line2() {
  if (g_lang == Lang::zh) {
    return " " + menu_item("q", "退出") + " ";
  } else {
    return " " + menu_item("q", "Quit") + " ";
  }
}

std::string info_filter_label(const std::string &tag_name) {
  if (g_lang == Lang::zh) {
    return "  筛选: " + tag_name;
  } else {
    return "  Filter: " + tag_name;
  }
}

std::string info_console_filter_label(const std::string &keyword) {
  if (g_lang == Lang::zh) {
    std::string label = keyword;
    if (keyword == "unevaluated") label = "未评估";
    else if (keyword == "fail") label = "评估不达标";
    else if (keyword == "reject") label = "废片";
    else if (keyword == "dup") label = "重复";
    return "  二级筛选: " + label;
  } else {
    return "  Filter2: " + keyword;
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

std::string info_source_label(bool is_raw) {
  if (g_lang == Lang::zh) {
    return is_raw ? "来源: RAW" : "来源: JPEG";
  } else {
    return is_raw ? "Source: RAW" : "Source: JPEG";
  }
}

// "拍摄时间: 2025-05-11 19:24" 这一整行经常超出信息栏这种窄列的宽度（比
// "大小:"/"来源:"那两行长不少），会被截断——改成跟"风格:"一样的"标题行 +
// 缩进值行"两行展示，标题（本函数）和格式化后的值（format_captured_at）
// 分开，调用方各自 emit 一行。
std::string info_captured_at_heading() {
  if (g_lang == Lang::zh) {
    return "拍摄时间:";
  } else {
    return "Captured:";
  }
}

std::string format_captured_at(std::optional<std::int64_t> captured_at) {
  if (!captured_at) return "-";
  // localtime_r 是 mktime 的逆运算——core::decode::read_jpeg_capture_time/
  // core::raw::read_capture_time 存进去的 epoch 都是按本地时区的
  // mktime() 语义算出来的，这里用 localtime_r 转回去，精确到分钟，正
  // 好能还原出跟拍摄时相机屏幕上、或者 EXIF 里原始字符串一致的墙钟时
  // 间，不会因为时区换算错位。
  time_t t = static_cast<time_t>(*captured_at);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return buf;
}

std::string info_style_label() {
  if (g_lang == Lang::zh) {
    return "风格:";
  } else {
    return "Recipe:";
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

std::string msg_console_filter_no_images() {
  if (g_lang == Lang::zh) {
    return " 没有符合条件的图片 ";
  } else {
    return " No images match this filter ";
  }
}

std::string msg_browse_exited() {
  if (g_lang == Lang::zh) {
    return "已退出浏览\n";
  } else {
    return "Exited browse mode\n";
  }
}

std::string export_current_success(const std::string &output_path, bool created_folder) {
  if (g_lang == Lang::zh) {
    std::string status = " 已导出到 '" + output_path + "'";
    if (created_folder) status += "(目录不存在,已新建)";
    status += " ";
    return status;
  } else {
    std::string status = " Exported to '" + output_path + "'";
    if (created_folder) status += " (created directory)";
    status += " ";
    return status;
  }
}

std::string export_current_skipped(const std::string &file_name, pzt::core::SkipReason reason) {
  if (g_lang == Lang::zh) {
    return " '" + file_name + "' 未导出:" + export_skip_reason(reason) + " ";
  } else {
    return " '" + file_name + "' not exported: " + export_skip_reason(reason) + " ";
  }
}

std::string msg_ai_prompt_placeholder() {
  if (g_lang == Lang::zh) {
    return "命令必须以 / 开头: /ai_eval [指引] | /ai_eval * | /ai_eval #标签 | /tasks | /dedup * | /dedup "
           "#标签 | /filter <条件> | /filter clear | /help";
  } else {
    return "Commands must start with /: /ai_eval [note] | /ai_eval * | /ai_eval #tag | /tasks | /dedup * "
           "| /dedup #tag | /filter <criterion> | /filter clear | /help";
  }
}

namespace {

// "+15%"/"-10%"这种带符号的百分比，%+ 格式说明符保证正数也带 "+" 号。
std::string format_signed_percent(double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%+.0f%%", value);
  return buf;
}

// "+2.5°"这种带符号的角度，保留一位小数——旋转角度这种量级下整数会丢掉
// 有意义的精度。
std::string format_signed_degrees(double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%+.1f\xC2\xB0", value);  // \xC2\xB0 是 ° 的 UTF-8 编码
  return buf;
}

}  // namespace

std::string evaluation_none_label() {
  if (g_lang == Lang::zh) {
    return "选片评估: 尚未评估";
  } else {
    return "Culling: not evaluated";
  }
}

std::string evaluation_summary_label(int overall_score, bool passes_gate) {
  std::string score_text = std::to_string(overall_score);
  if (g_lang == Lang::zh) {
    return "选片评估: " + score_text + "/10 · " + (passes_gate ? "达标" : "不达标");
  } else {
    return "Culling: " + score_text + "/10 · " + (passes_gate ? "PASS" : "FAIL");
  }
}

std::string evaluation_exposure_line(int score, const std::string& note,
                                      std::optional<double> fix_percent) {
  std::string suffix = fix_percent ? (g_lang == Lang::zh
                                           ? "（建议 " + format_signed_percent(*fix_percent) + "）"
                                           : " (suggest " + format_signed_percent(*fix_percent) + ")")
                                    : "";
  if (g_lang == Lang::zh) {
    return "曝光 " + std::to_string(score) + "/10: " + note + suffix;
  } else {
    return "Exposure " + std::to_string(score) + "/10: " + note + suffix;
  }
}

std::string evaluation_composition_line(int score, const std::string& note,
                                         std::optional<double> rotate_degrees) {
  std::string suffix =
      rotate_degrees ? (g_lang == Lang::zh
                             ? "（建议旋转 " + format_signed_degrees(*rotate_degrees) + "）"
                             : " (suggest rotate " + format_signed_degrees(*rotate_degrees) + ")")
                      : "";
  if (g_lang == Lang::zh) {
    return "构图 " + std::to_string(score) + "/10: " + note + suffix;
  } else {
    return "Composition " + std::to_string(score) + "/10: " + note + suffix;
  }
}

std::string evaluation_focus_line(int score, const std::string& note) {
  if (g_lang == Lang::zh) {
    return "对焦 " + std::to_string(score) + "/10: " + note;
  } else {
    return "Focus " + std::to_string(score) + "/10: " + note;
  }
}

std::string msg_ai_processing_pending() {
  if (g_lang == Lang::zh) {
    return " AI 处理中，请稍后 ";
  } else {
    return " AI request already in progress, please wait ";
  }
}

std::string msg_ai_processing_submitted() {
  if (g_lang == Lang::zh) {
    return " AI 处理请求已提交 ";
  } else {
    return " AI processing request submitted ";
  }
}

// F-03：把 EvaluationError 翻译成一句人话，不逐条列出的都归到笼统的
// "请求失败"——这几种原因(网络/HttpError/key)对用户来说都是"这次没请
// 求成功，再试一次"，不需要精确到协议层细节；ParseError/OutOfRange 单
// 独说明是"模型返回的内容不对"，跟"网络/权限"是不同性质的问题，值得
// 区分；ImageUnavailable 单独说明是"这张图暂时评估不了"，不是网络问
// 题，重试大概率还是不行(比如预览图解码失败)。
std::string ai_evaluation_error_reason(pzt::core::EvaluationError error) {
  switch (error) {
    case pzt::core::EvaluationError::MissingApiKey:
      return g_lang == Lang::zh ? "未配置 API key" : "API key not configured";
    case pzt::core::EvaluationError::NetworkError:
      return g_lang == Lang::zh ? "网络请求失败" : "network request failed";
    case pzt::core::EvaluationError::HttpError:
      return g_lang == Lang::zh ? "服务端返回错误" : "server returned an error";
    case pzt::core::EvaluationError::ParseError:
      return g_lang == Lang::zh ? "模型返回内容无法解析" : "couldn't parse the model's response";
    case pzt::core::EvaluationError::OutOfRange:
      return g_lang == Lang::zh ? "模型返回的分数超出范围" : "model returned an out-of-range score";
    case pzt::core::EvaluationError::ImageUnavailable:
      return g_lang == Lang::zh ? "图片暂时无法评估" : "image is currently unavailable";
    case pzt::core::EvaluationError::StorageFailed:
      return g_lang == Lang::zh ? "结果未能保存" : "failed to save the result";
  }
  return g_lang == Lang::zh ? "未知错误" : "unknown error";  // 不可达，安抚 -Wreturn-type
}

std::string msg_ai_evaluation_failed(pzt::core::ImageId image_id, pzt::core::EvaluationError error) {
  std::string reason = ai_evaluation_error_reason(error);
  if (g_lang == Lang::zh) {
    return " 图 " + std::to_string(image_id) + " 评估失败: " + reason + " ";
  } else {
    return " image " + std::to_string(image_id) + " evaluation failed: " + reason + " ";
  }
}

std::string msg_ai_unknown_command(const std::string &command) {
  if (g_lang == Lang::zh) {
    return " 未知命令: /" + command + " ";
  } else {
    return " Unknown command: /" + command + " ";
  }
}

std::string msg_help_overview() {
  if (g_lang == Lang::zh) {
    return " 可用命令: /ai_eval /dedup /tasks /filter /help —— /help <命令> 查看详情 ";
  } else {
    return " Available commands: /ai_eval /dedup /tasks /filter /help — /help <command> for details ";
  }
}

std::optional<std::string> msg_help_command(const std::string &command) {
  if (g_lang == Lang::zh) {
    if (command == "ai_eval") {
      return " /ai_eval [指引] 评估当前图片；/ai_eval * [指引] 评估全部；/ai_eval #标签 [指引] 评估该标签范围 ";
    }
    if (command == "dedup") {
      return " /dedup * 或 /dedup #标签：在范围内查找近似重复，非保留项打上\"重复\"标签 ";
    }
    if (command == "tasks") {
      return " /tasks：查看评估队列排队中/处理中的数量 ";
    }
    if (command == "filter") {
      return " /filter unevaluated|fail|reject|dup：在当前视图上再筛一层；/filter clear：清除 ";
    }
    if (command == "help") {
      return " /help [命令名]：不带参数列出全部命令，带参数显示该命令的详细用法 ";
    }
    return std::nullopt;
  } else {
    if (command == "ai_eval") {
      return " /ai_eval [note] evaluates the current photo; /ai_eval * [note] the whole project; "
             "/ai_eval #tag [note] a tag's scope ";
    }
    if (command == "dedup") {
      return " /dedup * or /dedup #tag: find near-duplicates in scope, tag non-keep members Duplicate ";
    }
    if (command == "tasks") {
      return " /tasks: shows how many evaluations are queued/processing ";
    }
    if (command == "filter") {
      return " /filter unevaluated|fail|reject|dup: narrows the current view further; /filter clear resets it ";
    }
    if (command == "help") {
      return " /help [command]: lists all commands, or shows detailed usage for one ";
    }
    return std::nullopt;
  }
}

std::string err_help_unknown_command(const std::string &command) {
  if (g_lang == Lang::zh) {
    return " 没有 '" + command + "' 这个命令的帮助 ";
  } else {
    return " No help available for '" + command + "' ";
  }
}

std::string err_console_tag_not_found(const std::string &tag_name) {
  if (g_lang == Lang::zh) {
    return " 找不到标签 '" + tag_name + "' ";
  } else {
    return " Tag '" + tag_name + "' not found ";
  }
}

std::string err_console_invalid_scope() {
  if (g_lang == Lang::zh) {
    return " 范围必须是 * 或 #标签名 ";
  } else {
    return " Scope must be * or #tag ";
  }
}

std::string err_console_invalid_filter_criterion() {
  if (g_lang == Lang::zh) {
    return " 筛选条件必须是 unevaluated/fail/reject/dup 之一 ";
  } else {
    return " Filter criterion must be one of unevaluated/fail/reject/dup ";
  }
}

std::string msg_console_requires_slash() {
  if (g_lang == Lang::zh) {
    return " 命令必须以 / 开头，输入 /help 查看命令列表 ";
  } else {
    return " Commands must start with /, type /help for a list ";
  }
}

std::string msg_ai_eval_submitted(int count) {
  if (count == 0) {
    if (g_lang == Lang::zh) {
      return " 没有需要评估的图片(都已经评估过了) ";
    } else {
      return " No images need evaluation (all already evaluated) ";
    }
  }
  if (g_lang == Lang::zh) {
    return " 已提交 " + std::to_string(count) + " 张图片的评估请求 ";
  } else {
    return " Submitted " + std::to_string(count) + " image(s) for evaluation ";
  }
}

std::string msg_ai_tasks_status(std::size_t queued, bool processing) {
  if (g_lang == Lang::zh) {
    std::string line = " 排队中: " + std::to_string(queued) + " 张";
    line += processing ? "，有一张正在处理 " : "，当前没有正在处理的 ";
    return line;
  } else {
    std::string line = " Queued: " + std::to_string(queued);
    line += processing ? ", one in progress " : ", nothing in progress ";
    return line;
  }
}

std::string msg_dedup_confirm_unevaluated_line1(int unevaluated_count) {
  if (g_lang == Lang::zh) {
    return " " + std::to_string(unevaluated_count) + " 张照片还没评估，保留判断会退化成按拍摄时间选 ";
  } else {
    return " " + std::to_string(unevaluated_count) +
           " image(s) not evaluated, keep-selection falls back to capture time ";
  }
}

std::string msg_dedup_confirm_unevaluated_line2() {
  if (g_lang == Lang::zh) {
    return " " + menu_item("y", "继续") + " / " + menu_item("其它键", "取消") + " ";
  } else {
    return " " + menu_item("y", "Continue") + " / " + menu_item("other keys", "Cancel") + " ";
  }
}

std::string msg_dedup_result(int group_count, int tagged_count, int skipped_no_capture_time) {
  // F-11：标记数为 0 时(没有新组、或范围内已经全部标记过)不给入口提
  // 示——用户按 g 9 只会看到空列表，反而更困惑。
  std::string hint = tagged_count > 0 ? (g_lang == Lang::zh ? "，按 g 9 查看" : ", press g 9 to view") : "";
  // F-08：以前这批图片被静默排除在比对之外，分组结果不如预期时用户无
  // 从判断原因——只在真的有跳过时才提一句，不干扰最常见的"全部图片都
  // 有拍摄时间"路径。
  std::string skipped_note =
      skipped_no_capture_time > 0
          ? (g_lang == Lang::zh
                 ? "，" + std::to_string(skipped_no_capture_time) + " 张因无拍摄时间未参与比对"
                 : ", " + std::to_string(skipped_no_capture_time) + " image(s) skipped (no capture time)")
          : "";
  if (g_lang == Lang::zh) {
    return " 找到 " + std::to_string(group_count) + " 组重复，标记了 " + std::to_string(tagged_count) +
           " 张" + skipped_note + hint + " ";
  } else {
    return " Found " + std::to_string(group_count) + " duplicate group(s), tagged " +
           std::to_string(tagged_count) + " image(s)" + skipped_note + hint + " ";
  }
}

std::string err_dedup_failed() {
  if (g_lang == Lang::zh) {
    return " 重复检测失败，请重试 ";
  } else {
    return " Duplicate detection failed, please try again ";
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
tag_menu_options_line(const std::vector<pzt::core::TagSummary> &tags, bool show_duplicate) {
  std::string line = " " + menu_item("0", reject_tag_label());
  for (size_t i = 0; i < tags.size(); ++i) {
    line += "  " + menu_item(std::to_string(i + 1), tags[i].name);
    if (tags[i].cap) {
      line += "(" + std::to_string(tags[i].tagged_count) + "/" +
              std::to_string(*tags[i].cap) + ")";
    }
  }
  if (show_duplicate) {
    line += "  " + menu_item("9", duplicate_tag_label());
  }
  return line;
}

std::string tag_menu_actions_line() {
  if (g_lang == Lang::zh) {
    return " " + menu_item("c", "新建") + "  " + menu_item("d", "删除") + "  " +
           menu_item("-", "摘除") + "  " + menu_item("Esc", "取消");
  } else {
    return " " + menu_item("c", "New") + "  " + menu_item("d", "Delete") + "  " +
           menu_item("-", "Remove") + "  " + menu_item("Esc", "Cancel");
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
filter_menu_options_line(const std::vector<pzt::core::TagSummary> &tags, bool show_duplicate) {
  std::string line = " " + menu_item("0", reject_tag_label());
  for (size_t i = 0; i < tags.size(); ++i) {
    line += "  " + menu_item(std::to_string(i + 1), tags[i].name);
  }
  if (show_duplicate) {
    line += "  " + menu_item("9", duplicate_tag_label());
  }
  return line;
}

std::string filter_menu_actions_line() {
  if (g_lang == Lang::zh) {
    return " " + menu_item("g", "清除筛选") + "  " + menu_item("e", "导出") + "  " +
           menu_item("Esc", "取消");
  } else {
    return " " + menu_item("g", "Clear Filter") + "  " + menu_item("e", "Export") + "  " +
           menu_item("Esc", "Cancel");
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
recipe_menu_options_line(const std::vector<pzt::core::PresetSummary> &presets) {
  std::string line;
  for (size_t i = 0; i < presets.size(); ++i) {
    if (i > 0) line += "  ";
    line += menu_item(std::to_string(i + 1), presets[i].name);
  }
  return " " + line;
}

std::string recipe_menu_actions_line(bool has_recipe) {
  if (g_lang == Lang::zh) {
    std::string line = " " + menu_item("r", "清除");
    if (has_recipe) line += "  " + menu_item("v", "切换原图/风格化");
    line += "  " + menu_item("c", "新建") + "  " + menu_item("d", "删除") + "  " +
            menu_item("Esc", "取消");
    return line;
  } else {
    std::string line = " " + menu_item("r", "Clear");
    if (has_recipe) line += "  " + menu_item("v", "Toggle Original/Style");
    line += "  " + menu_item("c", "New") + "  " + menu_item("d", "Delete") + "  " +
            menu_item("Esc", "Cancel");
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
