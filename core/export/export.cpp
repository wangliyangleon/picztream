#include "core/export/export.h"

#include <filesystem>
#include <iomanip>
#include <sstream>

#include "core/browse/browse.h"
#include "core/db/stmt.h"

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

}  // namespace

Result<ExportResult, ExportTagError> export_tag(db::Database& db, TagId tag_id,
                                                 const std::string& output_folder,
                                                 LinkMode link_mode) {
  sqlite3* conn = db.handle();

  auto tag_info = get_tag_info(conn, tag_id);
  if (!tag_info) {
    return Result<ExportResult, ExportTagError>::Err(ExportTagError::TagNotFound);
  }

  auto filtered = browse::filter_by_tag(db, tag_id);
  if (!filtered.ok()) {
    return Result<ExportResult, ExportTagError>::Err(ExportTagError::TagNotFound);
  }
  const auto& images = filtered.value();

  fs::path root_path = get_project_root_path(conn, tag_info->project_id);
  fs::path out_dir(output_folder);
  fs::create_directories(out_dir);

  const int width = zero_pad_width(images.size());

  ExportResult result;
  result.exported_count = 0;

  int index = 0;
  for (const auto& img : images) {
    ++index;
    fs::path source = root_path / img.file_path;
    if (!fs::exists(source)) {
      result.skipped.push_back(ExportSkipped{img.id, img.file_name, "源文件缺失"});
      continue;
    }

    std::string base_name =
        tag_info->is_ordered ? ordered_name(index, width, img.file_name) : img.file_name;
    fs::path target = resolve_collision(out_dir, base_name);

    if (link_mode == LinkMode::Copy) {
      fs::copy_file(source, target);
    } else {
      fs::create_symlink(fs::absolute(source), target);
    }
    ++result.exported_count;
  }

  return Result<ExportResult, ExportTagError>::Ok(std::move(result));
}

}  // namespace pzt::core::exporting
