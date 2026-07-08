#include "core/project/project.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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

std::string lower_ext(const fs::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return ext;
}

bool is_jpeg(const fs::path& p) {
  std::string ext = lower_ext(p);
  return ext == ".jpg" || ext == ".jpeg";
}

// M2：目前只认徕卡 DNG / 富士 RAF（docs/M2_PRD.md 明确的范围），写成一个
// 集合而不是分散的字符串比较，给"以后加 CR2/CR3/NEF/ARW"这个已知的未来
// 考虑留好扩展点，不需要在多处改判断逻辑。
constexpr std::array<const char*, 2> kRawExtensions = {".dng", ".raf"};

bool is_raw(const fs::path& p) {
  std::string ext = lower_ext(p);
  for (const char* raw_ext : kRawExtensions) {
    if (ext == raw_ext) return true;
  }
  return false;
}

struct ScannedImage {
  std::string relative_path;
  std::string file_name;
  std::int64_t file_size;
  std::string kind;                              // "jpeg" | "raw" | "raw_jpeg"
  std::optional<std::string> raw_relative_path;  // kind != "jpeg" 时有值
};

// 一个还没配对的 RAW 文件候选，扫描阶段的中间状态，不对外暴露。
struct RawFileEntry {
  std::string relative_path;
  std::string file_name;
  std::int64_t file_size;
};

// "目录 + 文件名主干(不含扩展名)"作为配对键。只有扩展名判断是大小写不敏
// 感的(is_jpeg/is_raw 已经做了)，主干本身按原样比较，不做大小写归一——
// 相机产出的文件名主干在实践中大小写是一致的，没必要引入额外的模糊匹配。
std::string path_stem_key(const fs::path& relative_path) {
  return (relative_path.parent_path() / relative_path.stem()).string();
}

// 递归扫描出 JPEG 和 RAW 两类文件，按"目录+文件名主干"分组配对：
// - 一个 JPEG + 一个 RAW 同主干 -> 合并成一条 kind="raw_jpeg"，取 JPEG 的
//   路径/文件名/大小(culling 预览用 JPEG，见 M2_Eng_Design.md)，raw_
//   relative_path 记 RAW 那份路径
// - 只有 JPEG -> kind="jpeg"（M0/M1 现有行为，不变）
// - 只有 RAW -> kind="raw"
// - 同一主干出现多个 RAW 文件（不该发生，但不强行消歧）：不配对，每个各
//   自独立成一条 kind="raw" 记录
std::vector<ScannedImage> scan_media(const fs::path& root) {
  std::vector<ScannedImage> jpegs;
  std::unordered_map<std::string, std::size_t> jpeg_index_by_stem;
  std::unordered_map<std::string, std::vector<RawFileEntry>> raw_groups;

  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    fs::path rel = fs::relative(entry.path(), root);
    if (is_jpeg(entry.path())) {
      jpeg_index_by_stem[path_stem_key(rel)] = jpegs.size();
      jpegs.push_back(ScannedImage{
          rel.string(),
          entry.path().filename().string(),
          static_cast<std::int64_t>(entry.file_size()),
          "jpeg",
          std::nullopt,
      });
    } else if (is_raw(entry.path())) {
      raw_groups[path_stem_key(rel)].push_back(RawFileEntry{
          rel.string(),
          entry.path().filename().string(),
          static_cast<std::int64_t>(entry.file_size()),
      });
    }
  }

  std::vector<ScannedImage> found = std::move(jpegs);
  for (auto& [stem, raws] : raw_groups) {
    if (raws.size() == 1) {
      auto jpeg_it = jpeg_index_by_stem.find(stem);
      if (jpeg_it != jpeg_index_by_stem.end()) {
        found[jpeg_it->second].kind = "raw_jpeg";
        found[jpeg_it->second].raw_relative_path = raws[0].relative_path;
        continue;
      }
      found.push_back(ScannedImage{
          raws[0].relative_path,
          raws[0].file_name,
          raws[0].file_size,
          "raw",
          raws[0].relative_path,
      });
    } else {
      for (const auto& raw : raws) {
        found.push_back(ScannedImage{
            raw.relative_path,
            raw.file_name,
            raw.file_size,
            "raw",
            raw.relative_path,
        });
      }
    }
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

// create_project 和 rescan_project 的 INSERT 语句都要写 kind/raw_path 这
// 两列，抽出来避免重复。
void bind_kind_and_raw_path(sqlite3_stmt* stmt, int kind_index, int raw_path_index,
                             const ScannedImage& img) {
  sqlite3_bind_text(stmt, kind_index, img.kind.c_str(), -1, SQLITE_TRANSIENT);
  if (img.raw_relative_path) {
    sqlite3_bind_text(stmt, raw_path_index, img.raw_relative_path->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, raw_path_index);
  }
}

}  // namespace

Result<ProjectId, CreateProjectError> create_project(db::Database& db, const std::string& name,
                                                       const std::string& folder_path) {
  sqlite3* conn = db.handle();

  if (project_name_exists(conn, name)) {
    return Result<ProjectId, CreateProjectError>::Err(CreateProjectError::NameAlreadyExists);
  }

  std::vector<ScannedImage> images = scan_media(fs::path(folder_path));
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
                       "imported_at, kind, raw_path) VALUES (?, ?, ?, ?, ?, ?, ?);");
    for (const auto& img : images) {
      sqlite3_reset(insert_image.get());
      sqlite3_bind_int64(insert_image.get(), 1, project_id);
      sqlite3_bind_text(insert_image.get(), 2, img.relative_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_image.get(), 3, img.file_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(insert_image.get(), 4, img.file_size);
      sqlite3_bind_int64(insert_image.get(), 5, created_at);
      bind_kind_and_raw_path(insert_image.get(), 6, 7, img);
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
  Stmt stmt(db.handle(),
            "SELECT id, project_id, file_path, file_name, file_size, kind, raw_path "
            "FROM images WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;

  ImageInfo info;
  info.id = sqlite3_column_int64(stmt.get(), 0);
  info.project_id = sqlite3_column_int64(stmt.get(), 1);
  info.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
  info.file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
  info.file_size = sqlite3_column_int64(stmt.get(), 4);
  info.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
  if (sqlite3_column_type(stmt.get(), 6) != SQLITE_NULL) {
    info.raw_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
  }
  return info;
}

Result<RescanSummary, ProjectNotFoundError> rescan_project(db::Database& db, ProjectId id,
                                                             bool prune) {
  sqlite3* conn = db.handle();
  auto summary = get_project_summary(conn, id);
  if (!summary) {
    return Result<RescanSummary, ProjectNotFoundError>::Err(ProjectNotFoundError::NotFound);
  }

  std::vector<ScannedImage> scanned = scan_media(fs::path(summary->root_path));
  const std::int64_t imported_at = now_unix();

  std::unordered_set<std::string> scanned_paths;
  scanned_paths.reserve(scanned.size());
  for (const auto& img : scanned) scanned_paths.insert(img.relative_path);

  std::int64_t added = 0;
  std::int64_t removed = 0;
  std::int64_t paired = 0;
  exec_simple(conn, "BEGIN;");
  try {
    Stmt find_by_path_stmt(conn,
                            "SELECT id, kind FROM images WHERE project_id = ? AND file_path = ?;");
    Stmt insert_stmt(conn,
                      "INSERT INTO images (project_id, file_path, file_name, file_size, "
                      "imported_at, kind, raw_path) VALUES (?, ?, ?, ?, ?, ?, ?);");
    // 把一条已有记录原地升级成配对：file_path/file_name 换成 JPEG 那份(配
    // 对约定 culling 预览用 JPEG，见 M2_Eng_Design.md)，kind 固定
    // "raw_jpeg"，raw_path 记 RAW 那份路径。两种升级方向(先有 JPEG 后补
    // RAW / 先有 RAW 后补 JPEG)用的是同一条语句，只是调用时传的
    // existing_id 来自不同的查找。
    Stmt upgrade_stmt(conn,
                       "UPDATE images SET file_path = ?, file_name = ?, kind = 'raw_jpeg', "
                       "raw_path = ? WHERE id = ?;");

    auto find_by_path = [&](const std::string& path) -> std::optional<std::pair<ImageId, std::string>> {
      sqlite3_reset(find_by_path_stmt.get());
      sqlite3_bind_int64(find_by_path_stmt.get(), 1, id);
      sqlite3_bind_text(find_by_path_stmt.get(), 2, path.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(find_by_path_stmt.get()) != SQLITE_ROW) return std::nullopt;
      return std::make_pair(
          sqlite3_column_int64(find_by_path_stmt.get(), 0),
          std::string(reinterpret_cast<const char*>(sqlite3_column_text(find_by_path_stmt.get(), 1))));
    };

    auto run_upgrade = [&](ImageId existing_id, const ScannedImage& img) {
      sqlite3_reset(upgrade_stmt.get());
      sqlite3_bind_text(upgrade_stmt.get(), 1, img.relative_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(upgrade_stmt.get(), 2, img.file_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(upgrade_stmt.get(), 3, img.raw_relative_path->c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(upgrade_stmt.get(), 4, existing_id);
      if (sqlite3_step(upgrade_stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string("upgrade image failed: ") + sqlite3_errmsg(conn));
      }
    };

    for (const auto& img : scanned) {
      if (auto existing = find_by_path(img.relative_path)) {
        // 已经有一条记录用的就是这个 file_path——纯 JPEG/纯 RAW 情况下这就
        // 是它自己，什么都不用做；如果这次扫描把它识别成了配对
        // (kind="raw_jpeg")而记录还停留在旧的单一 kind，原地升级。
        if (img.kind == "raw_jpeg" && existing->second != "raw_jpeg") {
          run_upgrade(existing->first, img);
          ++paired;
        }
        continue;
      }

      if (img.kind == "raw_jpeg") {
        // JPEG 路径没匹配到已有记录，可能是 RAW 那一半之前被当纯 RAW 单独
        // 记过——按 RAW 路径再查一次。
        if (auto existing_raw = find_by_path(*img.raw_relative_path)) {
          run_upgrade(existing_raw->first, img);
          ++paired;
          continue;
        }
      }

      sqlite3_reset(insert_stmt.get());
      sqlite3_bind_int64(insert_stmt.get(), 1, id);
      sqlite3_bind_text(insert_stmt.get(), 2, img.relative_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_stmt.get(), 3, img.file_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(insert_stmt.get(), 4, img.file_size);
      sqlite3_bind_int64(insert_stmt.get(), 5, imported_at);
      bind_kind_and_raw_path(insert_stmt.get(), 6, 7, img);
      if (sqlite3_step(insert_stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string("insert image failed: ") + sqlite3_errmsg(conn));
      }
      ++added;
    }

    if (prune) {
      // images 对 image_tags 是 ON DELETE CASCADE（core/db/schema.cpp 已经
      // PRAGMA foreign_keys = ON，delete_project 依赖的是同一个级联行为），
      // 删这一行就会把这张图打过的标签一并带走，不需要额外手写删
      // image_tags 的语句。
      std::vector<std::pair<std::int64_t, std::string>> existing;
      Stmt list_stmt(conn, "SELECT id, file_path FROM images WHERE project_id = ?;");
      sqlite3_bind_int64(list_stmt.get(), 1, id);
      while (sqlite3_step(list_stmt.get()) == SQLITE_ROW) {
        existing.emplace_back(
            sqlite3_column_int64(list_stmt.get(), 0),
            reinterpret_cast<const char*>(sqlite3_column_text(list_stmt.get(), 1)));
      }

      Stmt delete_stmt(conn, "DELETE FROM images WHERE id = ?;");
      for (const auto& [image_id, file_path] : existing) {
        if (scanned_paths.count(file_path) != 0) continue;  // 磁盘上还在
        sqlite3_reset(delete_stmt.get());
        sqlite3_bind_int64(delete_stmt.get(), 1, image_id);
        if (sqlite3_step(delete_stmt.get()) != SQLITE_DONE) {
          throw std::runtime_error(std::string("delete missing image failed: ") +
                                    sqlite3_errmsg(conn));
        }
        ++removed;
      }
    }

    exec_simple(conn, "COMMIT;");
  } catch (...) {
    exec_simple(conn, "ROLLBACK;");
    throw;
  }

  RescanSummary result;
  result.added_count = added;
  result.removed_count = removed;
  result.total_count = summary->image_count + added - removed;
  result.paired_count = paired;
  return Result<RescanSummary, ProjectNotFoundError>::Ok(result);
}

}  // namespace pzt::core::project
