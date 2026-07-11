#include "core/export/export.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>

#include "core/browse/browse.h"
#include "core/db/stmt.h"
#include "core/decode/decode.h"
#include "core/recipe/recipe.h"

namespace pzt::core::exporting {

namespace fs = std::filesystem;

namespace {

using db::Stmt;
using project::ProjectId;

struct TagInfo {
  ProjectId project_id;
  bool is_ordered;
};

std::optional<TagInfo> get_tag_info(sqlite3* conn, TagId tag_id) {
  Stmt stmt(conn, "SELECT project_id, is_ordered FROM tags WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, tag_id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;

  TagInfo info;
  info.project_id = sqlite3_column_int64(stmt.get(), 0);
  info.is_ordered = sqlite3_column_int64(stmt.get(), 1) != 0;
  return info;
}

std::string get_project_root_path(sqlite3* conn, ProjectId project_id) {
  Stmt stmt(conn, "SELECT root_path FROM projects WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, project_id);
  sqlite3_step(stmt.get());  // tag_info 存在意味着它的 project 一定存在（外键）
  return reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
}

int zero_pad_width(std::size_t count) {
  int digits = 1;
  while (count >= 10) {
    count /= 10;
    ++digits;
  }
  return std::max(2, digits);
}

std::string ordered_name(int index, int width, const std::string& file_name) {
  std::ostringstream oss;
  oss << std::setw(width) << std::setfill('0') << index << "_" << file_name;
  return oss.str();
}

// 目标文件名冲突时在扩展名前追加 _2、_3……直到不冲突。每次导出都立刻落盘
// (复制或建符号链接)，所以直接对着文件系统实时状态判断冲突就够了，不需要
// 额外维护一份"本次已用文件名"的内存集合。
fs::path resolve_collision(const fs::path& dir, const std::string& base_name) {
  fs::path candidate = dir / base_name;
  if (!fs::exists(candidate)) return candidate;

  fs::path stem = fs::path(base_name).stem();
  fs::path ext = fs::path(base_name).extension();
  for (int suffix = 2;; ++suffix) {
    fs::path try_path = dir / (stem.string() + "_" + std::to_string(suffix) + ext.string());
    if (!fs::exists(try_path)) return try_path;
  }
}

// 单张图片的实际写出逻辑：kind × 有无 recipe 四态路由。export_tag 和
// export_image 共用同一份，只接收写出逻辑需要的字段，不接收整个
// ImageInfo/ImageRef——两个调用方用的是不同的图片结构体，字段有重叠但不
// 同源，取需要的字段比硬统一类型更简单。调用方负责在调用前确认 source
// 存在、算好 target（含冲突消歧），以及 kind="raw" 时的进度回调时机（要
// 在这个函数调用之前，不是之后，见函数说明）。
std::optional<SkipReason> write_one_export(db::Database& db, ImageId image_id,
                                            const std::string& kind, const fs::path& source,
                                            const fs::path& target,
                                            const RawDecodeFn& raw_decode_fn) {
  auto recipe_id = recipe::get_image_recipe(db, image_id);

  if (kind == "raw") {
    // 全量解码：不管有没有 recipe 都要走这一步（M2_PRD.md 已经定了 RAW
    // 图片导出永远落地成可直接查看的 JPEG），只是有没有 recipe 决定后面
    // 要不要再调 render。
    auto decoded = raw_decode_fn(source.string());
    if (!decoded.ok()) return SkipReason::RawDecodeFailed;
    decode::DecodedImage final_image = std::move(decoded.value());
    if (recipe_id) {
      auto rendered = recipe::render(db, final_image, *recipe_id, std::thread::hardware_concurrency());
      if (!rendered.ok()) return SkipReason::RenderFailed;
      final_image = std::move(rendered.value());
    }
    auto encoded = decode::encode_jpeg_file(final_image, target.string());
    if (!encoded.ok()) return SkipReason::EncodeFailed;
  } else if (recipe_id) {
    // 全分辨率烘焙：解码原图 -> recipe::render(多线程) -> 编码写出，取代
    // 复制。
    auto decoded = decode::decode_jpeg_file(source.string());
    if (!decoded.ok()) return SkipReason::DecodeFailed;
    auto rendered = recipe::render(db, decoded.value(), *recipe_id, std::thread::hardware_concurrency());
    if (!rendered.ok()) return SkipReason::RenderFailed;
    auto encoded = decode::encode_jpeg_file(rendered.value(), target.string());
    if (!encoded.ok()) return SkipReason::EncodeFailed;
  } else {
    // 没有应用 recipe 的 JPEG 图片继续走原来的复制，字节级不变。
    fs::copy_file(source, target);
  }
  return std::nullopt;
}

// export_tag/export_image 都要用："kind=raw 的图片导出永远落地成 .jpg，
// 目标文件名不能直接沿用 .dng/.raf 原扩展名"这条规则只有一份。
std::string target_file_name(const std::string& file_name, const std::string& kind) {
  return kind == "raw" ? fs::path(file_name).replace_extension(".jpg").string() : file_name;
}

}  // namespace

Result<ExportResult, ExportTagError> export_tag(db::Database& db, TagId tag_id,
                                                 const std::string& output_folder,
                                                 ExportProgressFn on_progress,
                                                 RawDecodeFn raw_decode_fn, bool include_reject,
                                                 bool include_dup) {
  sqlite3* conn = db.handle();

  auto tag_info = get_tag_info(conn, tag_id);
  if (!tag_info) {
    return Result<ExportResult, ExportTagError>::Err(ExportTagError::TagNotFound);
  }

  auto filtered = browse::filter_by_tag(db, tag_id);
  if (!filtered.ok()) {
    return Result<ExportResult, ExportTagError>::Err(ExportTagError::TagNotFound);
  }
  auto images = std::move(filtered.value());

  // F-26：默认排除废片/重复，除非调用方显式要求包含，或者这次导出的
  // 目标标签本身就是废片/重复(用户已经明确要处理它)。项目里还没有对
  // 应系统标签时(find_tag_by_name 查不到)没有可排除的东西，跳过。
  auto exclude_by_tag = [&](const char* system_tag_name) {
    auto system_tag_id = tagging::find_tag_by_name(db, tag_info->project_id, system_tag_name);
    if (!system_tag_id || tag_id == *system_tag_id) return;
    std::vector<ImageId> ids;
    ids.reserve(images.size());
    for (const auto& img : images) ids.push_back(img.id);
    auto matched = tagging::images_with_tag(db, ids, *system_tag_id);
    if (matched.empty()) return;
    images.erase(std::remove_if(images.begin(), images.end(),
                                 [&](const auto& img) { return matched.count(img.id) > 0; }),
                 images.end());
  };
  if (!include_reject) exclude_by_tag(tagging::kRejectTagName);
  if (!include_dup) exclude_by_tag(tagging::kDuplicateTagName);

  fs::path root_path = get_project_root_path(conn, tag_info->project_id);
  fs::path out_dir(output_folder);

  const int width = zero_pad_width(images.size());

  ExportResult result;
  result.exported_count = 0;
  result.created_output_folder = false;

  // M2：这批图片里有多少张 kind="raw"（不管有没有 recipe，两条 raw 分支
  // 都要走 raw_decode_fn），作为进度回调的分母。纯 JPEG 批次 raw_total==0，
  // on_progress 全程不会被调用。
  int raw_total = 0;
  for (const auto& img : images) {
    if (img.kind == "raw") ++raw_total;
  }
  int raw_done = 0;

  // 目标文件夹无法创建/写入(权限不足、路径某一段已经是个普通文件、磁盘写
  // 满等)时,std::filesystem 的抛异常重载会往外抛 filesystem_error——不
  // 捕获的话会直接终止调用方(包括 cli 全键盘交互循环那个长时间运行的进
  // 程),这里统一转成 IoError,让调用方能把它当成普通的 Result 错误处理。
  try {
    // 导出前先看一眼目标是否已存在,再调用 create_directories——用这个结
    // 果告诉调用方"是不是新建的",不然用户拿到一句"已导出"完全看不出目标
    // 文件夹是本来就有的、还是这次顺手建的,容易以为自己打错了路径。
    result.created_output_folder = !fs::exists(out_dir);
    fs::create_directories(out_dir);

    int index = 0;
    for (const auto& img : images) {
      ++index;
      fs::path source = root_path / img.file_path;
      if (!fs::exists(source)) {
        result.skipped.push_back(ExportSkipped{img.id, img.file_name, SkipReason::SourceMissing});
        continue;
      }

      std::string file_name_for_target = target_file_name(img.file_name, img.kind);
      std::string base_name = tag_info->is_ordered
                                   ? ordered_name(index, width, file_name_for_target)
                                   : file_name_for_target;
      fs::path target = resolve_collision(out_dir, base_name);

      // 进度回调要在解码开始前触发，不是完成后——全量解码单张就要几秒，
      // 完成后才报进度的话，批次里第一张(单张导出时是唯一一张)在解码期
      // 间界面上什么都不显示，看起来像卡住了，真机使用中被发现过。
      if (img.kind == "raw") {
        ++raw_done;
        if (on_progress) on_progress(raw_done, raw_total);
      }
      auto skip_reason = write_one_export(db, img.id, img.kind, source, target, raw_decode_fn);
      if (skip_reason) {
        result.skipped.push_back(ExportSkipped{img.id, img.file_name, *skip_reason});
        continue;
      }
      ++result.exported_count;
    }
  } catch (const fs::filesystem_error&) {
    return Result<ExportResult, ExportTagError>::Err(ExportTagError::IoError);
  }

  return Result<ExportResult, ExportTagError>::Ok(std::move(result));
}

Result<ExportImageResult, ExportImageError> export_image(db::Database& db, ImageId image_id,
                                                           const std::string& output_folder,
                                                           ExportProgressFn on_progress,
                                                           RawDecodeFn raw_decode_fn) {
  sqlite3* conn = db.handle();

  auto img = project::get_image(db, image_id);
  if (!img) {
    return Result<ExportImageResult, ExportImageError>::Err(ExportImageError::ImageNotFound);
  }

  fs::path root_path = get_project_root_path(conn, img->project_id);
  fs::path out_dir(output_folder);
  fs::path source = root_path / img->file_path;

  ExportImageResult result;
  result.exported = false;
  result.created_output_folder = false;

  try {
    result.created_output_folder = !fs::exists(out_dir);
    fs::create_directories(out_dir);

    if (!fs::exists(source)) {
      result.skip_reason = SkipReason::SourceMissing;
      return Result<ExportImageResult, ExportImageError>::Ok(std::move(result));
    }

    // 单张导出没有"标签是否有序"这个概念，直接用原文件名（RAW 照样把扩
    // 展名换成 .jpg），只走冲突消歧。
    fs::path target = resolve_collision(out_dir, target_file_name(img->file_name, img->kind));

    if (img->kind == "raw" && on_progress) on_progress(1, 1);
    auto skip_reason = write_one_export(db, img->id, img->kind, source, target, raw_decode_fn);
    if (skip_reason) {
      result.skip_reason = skip_reason;
      return Result<ExportImageResult, ExportImageError>::Ok(std::move(result));
    }

    result.exported = true;
    result.output_path = target.string();
  } catch (const fs::filesystem_error&) {
    return Result<ExportImageResult, ExportImageError>::Err(ExportImageError::IoError);
  }

  return Result<ExportImageResult, ExportImageError>::Ok(std::move(result));
}

}  // namespace pzt::core::exporting
