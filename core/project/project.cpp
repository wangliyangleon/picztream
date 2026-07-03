#include "core/project/project.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include "core/db/stmt.h"

namespace pzt::core::project {

namespace fs = std::filesystem;

namespace {

using db::exec_simple;
using db::Stmt;

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool is_jpeg(const fs::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return ext == ".jpg" || ext == ".jpeg";
}

struct ScannedImage {
  std::string relative_path;
  std::string file_name;
  std::int64_t file_size;
};

std::vector<ScannedImage> scan_jpegs(const fs::path& root) {
  std::vector<ScannedImage> found;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    if (!is_jpeg(entry.path())) continue;
    found.push_back(ScannedImage{
        fs::relative(entry.path(), root).string(),
        entry.path().filename().string(),
        static_cast<std::int64_t>(entry.file_size()),
    });
  }
  return found;
}

bool project_name_exists(sqlite3* db, const std::string& name) {
  Stmt stmt(db, "SELECT 1 FROM projects WHERE name = ?;");
  sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

std::optional<ProjectId> find_id_by(sqlite3* db, const char* where_clause,
                                     const std::string& value) {
  std::string sql = std::string("SELECT id FROM projects WHERE ") + where_clause + ";";
  Stmt stmt(db, sql.c_str());
  sqlite3_bind_text(stmt.get(), 1, value.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
  return sqlite3_column_int64(stmt.get(), 0);
}

std::optional<ProjectSummary> get_project_summary(sqlite3* db, ProjectId id) {
  Stmt stmt(db,
            "SELECT p.id, p.name, p.root_path, p.archived_at, COUNT(i.id) "
            "FROM projects p LEFT JOIN images i ON i.project_id = p.id "
            "WHERE p.id = ? "
            "GROUP BY p.id;");
  sqlite3_bind_int64(stmt.get(), 1, id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;

  ProjectSummary s;
  s.id = sqlite3_column_int64(stmt.get(), 0);
  s.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
  s.root_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
  s.archived = sqlite3_column_type(stmt.get(), 3) != SQLITE_NULL;
  s.image_count = sqlite3_column_int64(stmt.get(), 4);
  return s;
}

}  // namespace

Result<ProjectId, CreateProjectError> create_project(db::Database& db, const std::string& name,
                                                       const std::string& folder_path) {
  sqlite3* conn = db.handle();

  if (project_name_exists(conn, name)) {
    return Result<ProjectId, CreateProjectError>::Err(CreateProjectError::NameAlreadyExists);
  }

  std::vector<ScannedImage> images = scan_jpegs(fs::path(folder_path));
  if (images.empty()) {
    return Result<ProjectId, CreateProjectError>::Err(CreateProjectError::NoImagesFound);
  }

  const std::int64_t created_at = now_unix();

  exec_simple(conn, "BEGIN;");
  try {
    Stmt insert_project(conn,
                         "INSERT INTO projects (name, root_path, created_at) VALUES (?, ?, ?);");
    sqlite3_bind_text(insert_project.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_project.get(), 2, folder_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_project.get(), 3, created_at);
    if (sqlite3_step(insert_project.get()) != SQLITE_DONE) {
      throw std::runtime_error(std::string("insert project failed: ") + sqlite3_errmsg(conn));
    }
    const ProjectId project_id = sqlite3_last_insert_rowid(conn);

    Stmt insert_image(conn,
                       "INSERT INTO images (project_id, file_path, file_name, file_size, "
                       "imported_at) VALUES (?, ?, ?, ?, ?);");
    for (const auto& img : images) {
      sqlite3_reset(insert_image.get());
      sqlite3_bind_int64(insert_image.get(), 1, project_id);
      sqlite3_bind_text(insert_image.get(), 2, img.relative_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_image.get(), 3, img.file_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(insert_image.get(), 4, img.file_size);
      sqlite3_bind_int64(insert_image.get(), 5, created_at);
      if (sqlite3_step(insert_image.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string("insert image failed: ") + sqlite3_errmsg(conn));
      }
    }

    exec_simple(conn, "COMMIT;");
    return Result<ProjectId, CreateProjectError>::Ok(project_id);
  } catch (...) {
    exec_simple(conn, "ROLLBACK;");
    throw;
  }
}

std::vector<ProjectSummary> list_projects(db::Database& db) {
  Stmt stmt(db.handle(),
            "SELECT p.id, p.name, p.root_path, p.archived_at, COUNT(i.id) "
            "FROM projects p LEFT JOIN images i ON i.project_id = p.id "
            "GROUP BY p.id "
            "ORDER BY (p.archived_at IS NULL) DESC, p.name ASC;");

  std::vector<ProjectSummary> out;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    ProjectSummary s;
    s.id = sqlite3_column_int64(stmt.get(), 0);
    s.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    s.root_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    s.archived = sqlite3_column_type(stmt.get(), 3) != SQLITE_NULL;
    s.image_count = sqlite3_column_int64(stmt.get(), 4);
    out.push_back(std::move(s));
  }
  return out;
}

std::optional<ProjectId> find_project_by_name(db::Database& db, const std::string& name) {
  return find_id_by(db.handle(), "name = ?", name);
}

std::optional<ProjectId> find_project_by_root_path(db::Database& db, const std::string& path) {
  return find_id_by(db.handle(), "root_path = ?", path);
}

Result<ProjectSummary, ProjectNotFoundError> open_project(db::Database& db, ProjectId id) {
  auto summary = get_project_summary(db.handle(), id);
  if (!summary) {
    return Result<ProjectSummary, ProjectNotFoundError>::Err(ProjectNotFoundError::NotFound);
  }
  return Result<ProjectSummary, ProjectNotFoundError>::Ok(std::move(*summary));
}

Result<void, ProjectNotFoundError> archive_project(db::Database& db, ProjectId id) {
  sqlite3* conn = db.handle();
  Stmt stmt(conn, "UPDATE projects SET archived_at = ? WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, now_unix());
  sqlite3_bind_int64(stmt.get(), 2, id);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("archive failed: ") + sqlite3_errmsg(conn));
  }
  if (sqlite3_changes(conn) == 0) {
    return Result<void, ProjectNotFoundError>::Err(ProjectNotFoundError::NotFound);
  }
  return Result<void, ProjectNotFoundError>::Ok();
}

Result<void, ProjectNotFoundError> delete_project(db::Database& db, ProjectId id) {
  sqlite3* conn = db.handle();
  Stmt stmt(conn, "DELETE FROM projects WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, id);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("delete failed: ") + sqlite3_errmsg(conn));
  }
  if (sqlite3_changes(conn) == 0) {
    return Result<void, ProjectNotFoundError>::Err(ProjectNotFoundError::NotFound);
  }
  return Result<void, ProjectNotFoundError>::Ok();
}

std::optional<ImageId> find_image_by_path(db::Database& db, ProjectId project_id,
                                           const std::string& relative_path) {
  Stmt stmt(db.handle(), "SELECT id FROM images WHERE project_id = ? AND file_path = ?;");
  sqlite3_bind_int64(stmt.get(), 1, project_id);
  sqlite3_bind_text(stmt.get(), 2, relative_path.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
  return sqlite3_column_int64(stmt.get(), 0);
}

std::optional<ImageInfo> get_image(db::Database& db, ImageId id) {
  Stmt stmt(db.handle(), "SELECT id, project_id, file_path, file_name FROM images WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;

  ImageInfo info;
  info.id = sqlite3_column_int64(stmt.get(), 0);
  info.project_id = sqlite3_column_int64(stmt.get(), 1);
  info.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
  info.file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
  return info;
}

Result<RescanSummary, ProjectNotFoundError> rescan_project(db::Database& db, ProjectId id) {
  sqlite3* conn = db.handle();
  auto summary = get_project_summary(conn, id);
  if (!summary) {
    return Result<RescanSummary, ProjectNotFoundError>::Err(ProjectNotFoundError::NotFound);
  }

  std::vector<ScannedImage> scanned = scan_jpegs(fs::path(summary->root_path));
  const std::int64_t imported_at = now_unix();

  std::int64_t added = 0;
  exec_simple(conn, "BEGIN;");
  try {
    Stmt exists_stmt(conn, "SELECT 1 FROM images WHERE project_id = ? AND file_path = ?;");
    Stmt insert_stmt(conn,
                      "INSERT INTO images (project_id, file_path, file_name, file_size, "
                      "imported_at) VALUES (?, ?, ?, ?, ?);");
    for (const auto& img : scanned) {
      sqlite3_reset(exists_stmt.get());
      sqlite3_bind_int64(exists_stmt.get(), 1, id);
      sqlite3_bind_text(exists_stmt.get(), 2, img.relative_path.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(exists_stmt.get()) == SQLITE_ROW) continue;  // 已经在 images 表里了

      sqlite3_reset(insert_stmt.get());
      sqlite3_bind_int64(insert_stmt.get(), 1, id);
      sqlite3_bind_text(insert_stmt.get(), 2, img.relative_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_stmt.get(), 3, img.file_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(insert_stmt.get(), 4, img.file_size);
      sqlite3_bind_int64(insert_stmt.get(), 5, imported_at);
      if (sqlite3_step(insert_stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string("insert image failed: ") + sqlite3_errmsg(conn));
      }
      ++added;
    }
    exec_simple(conn, "COMMIT;");
  } catch (...) {
    exec_simple(conn, "ROLLBACK;");
    throw;
  }

  RescanSummary result;
  result.added_count = added;
  result.total_count = summary->image_count + added;
  return Result<RescanSummary, ProjectNotFoundError>::Ok(result);
}

}  // namespace pzt::core::project
