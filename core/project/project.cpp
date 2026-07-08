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
#include "core/decode/decode.h"
#include "core/raw/raw.h"

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
  std::string kind;  // "jpeg" | "raw"
};

// 一个 RAW 文件候选，扫描阶段的中间状态，不对外暴露。
struct RawFileEntry {
  std::string relative_path;
  std::string file_name;
  std::int64_t file_size;
};

// "目录 + 文件名主干(不含扩展名)"用来判断"这个 JPEG 是不是应该被同名 RAW
// 挤掉"。只有扩展名判断是大小写不敏感的(is_jpeg/is_raw 已经做了)，主干本
// 身按原样比较，不做大小写归一——相机产出的文件名主干在实践中大小写是一
// 致的，没必要引入额外的模糊匹配。
std::string path_stem_key(const fs::path& relative_path) {
  return (relative_path.parent_path() / relative_path.stem()).string();
}

// 递归扫描出 JPEG 和 RAW 两类文件，按"目录+文件名主干"分组：
// - 一组里存在 RAW 文件(不管有没有同名 JPEG) -> 只产出 RAW 的记录，同名
//   JPEG 直接忽略、不生成任何记录(M2_Eng_Design.md"RAW+JPEG 同名")
// - 一组里只有 JPEG -> kind="jpeg"（M0/M1 现有行为，不变）
// - 同一主干出现多个 RAW 文件（不该发生，但不强行消歧）：每个各自独立成
//   一条 kind="raw" 记录
std::vector<ScannedImage> scan_media(const fs::path& root) {
  std::vector<ScannedImage> jpegs;
  std::unordered_set<std::string> raw_stems;
  std::unordered_map<std::string, std::vector<RawFileEntry>> raw_groups;

  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    fs::path rel = fs::relative(entry.path(), root);
    if (is_jpeg(entry.path())) {
      jpegs.push_back(ScannedImage{
          rel.string(),
          entry.path().filename().string(),
          static_cast<std::int64_t>(entry.file_size()),
          "jpeg",
      });
    } else if (is_raw(entry.path())) {
      std::string stem = path_stem_key(rel);
      raw_stems.insert(stem);
      raw_groups[stem].push_back(RawFileEntry{
          rel.string(),
          entry.path().filename().string(),
          static_cast<std::int64_t>(entry.file_size()),
      });
    }
  }

  std::vector<ScannedImage> found;
  for (auto& jpeg : jpegs) {
    if (raw_stems.count(path_stem_key(jpeg.relative_path)) != 0) continue;  // 被同名 RAW 挤掉
    found.push_back(std::move(jpeg));
  }
  for (auto& [stem, raws] : raw_groups) {
    for (const auto& raw : raws) {
      found.push_back(ScannedImage{raw.relative_path, raw.file_name, raw.file_size, "raw"});
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

// RAW 预览缓存的存放目录：跟这次打开的数据库文件同一个父目录下的
// raw_previews/<project_id> 子目录——不是硬编码 ~/.config/pzt/，这样测试
// 用 Database::open_at 指向 /tmp 下的临时库时，缓存也会落在 /tmp 里，不
// 会碰真实用户数据。见 core/db/database.h 的 Database::path() 说明。
fs::path raw_preview_cache_dir(db::Database& db, ProjectId project_id) {
  return fs::path(db.path()).parent_path() / "raw_previews" / std::to_string(project_id);
}

// 按 kind 分发到对应的拍摄时间提取函数——这里已经知道 kind 了，不需要像
// core::decode_preview_file 那样按扩展名猜。1-2ms 量级（open_file()/
// CGImageSourceCopyPropertiesAtIndex 都不触发全量解码），可以放心放进主
// 扫描路径，不需要像预览缓存生成那样挪到事务提交之后。
std::optional<std::int64_t> read_capture_time(const std::string& kind,
                                               const std::string& absolute_path) {
  return kind == "raw" ? raw::read_capture_time(absolute_path)
                        : decode::read_jpeg_capture_time(absolute_path);
}

// 给一张 RAW 图片生成降分辨率预览缓存并写盘，成功时返回缓存的绝对路径。
// 失败（LibRaw 解不出来、编码失败）时返回 nullopt，调用方不应该让这个失
// 败中断整批处理——照抄 core/export 里"单张失败不中断整体"的既有惯例。
std::optional<std::string> generate_preview_cache(db::Database& db, ProjectId project_id,
                                                    ImageId image_id,
                                                    const std::string& raw_absolute_path) {
  auto decoded = raw::decode_preview(raw_absolute_path);
  if (!decoded.ok()) return std::nullopt;

  fs::path cache_dir = raw_preview_cache_dir(db, project_id);
  std::error_code ec;
  fs::create_directories(cache_dir, ec);
  if (ec) return std::nullopt;

  fs::path cache_path = cache_dir / (std::to_string(image_id) + ".jpg");
  auto encoded = decode::encode_jpeg_file(decoded.value(), cache_path.string());
  if (!encoded.ok()) return std::nullopt;
  return cache_path.string();
}

}  // namespace

Result<ProjectId, CreateProjectError> create_project(db::Database& db, const std::string& name,
                                                       const std::string& folder_path,
                                                       ScanProgressFn on_progress) {
  sqlite3* conn = db.handle();

  if (project_name_exists(conn, name)) {
    return Result<ProjectId, CreateProjectError>::Err(CreateProjectError::NameAlreadyExists);
  }

  std::vector<ScannedImage> images = scan_media(fs::path(folder_path));
  if (images.empty()) {
    return Result<ProjectId, CreateProjectError>::Err(CreateProjectError::NoImagesFound);
  }

  const std::int64_t created_at = now_unix();
  ProjectId project_id = 0;
  std::vector<std::pair<ImageId, std::string>> needs_cache;  // (image_id, relative_path)

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
    project_id = sqlite3_last_insert_rowid(conn);

    Stmt insert_image(conn,
                       "INSERT INTO images (project_id, file_path, file_name, file_size, "
                       "imported_at, kind, captured_at) VALUES (?, ?, ?, ?, ?, ?, ?);");
    for (const auto& img : images) {
      auto captured_at =
          read_capture_time(img.kind, (fs::path(folder_path) / img.relative_path).string());

      sqlite3_reset(insert_image.get());
      sqlite3_bind_int64(insert_image.get(), 1, project_id);
      sqlite3_bind_text(insert_image.get(), 2, img.relative_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_image.get(), 3, img.file_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(insert_image.get(), 4, img.file_size);
      sqlite3_bind_int64(insert_image.get(), 5, created_at);
      sqlite3_bind_text(insert_image.get(), 6, img.kind.c_str(), -1, SQLITE_TRANSIENT);
      if (captured_at) {
        sqlite3_bind_int64(insert_image.get(), 7, *captured_at);
      } else {
        sqlite3_bind_null(insert_image.get(), 7);
      }
      if (sqlite3_step(insert_image.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string("insert image failed: ") + sqlite3_errmsg(conn));
      }
      if (img.kind == "raw") {
        needs_cache.emplace_back(sqlite3_last_insert_rowid(conn), img.relative_path);
      }
    }

    exec_simple(conn, "COMMIT;");
  } catch (...) {
    exec_simple(conn, "ROLLBACK;");
    throw;
  }

  // 预览缓存生成(慢，可能秒级/张)特意放在主事务提交之后：不拖长那次
  // BEGIN/COMMIT 持有的时间，单张生成失败也不影响已经成功创建的项目和其
  // 它图片的记录。
  Stmt update_cache_stmt(conn, "UPDATE images SET preview_cache_path = ? WHERE id = ?;");
  int done = 0;
  for (const auto& [image_id, relative_path] : needs_cache) {
    // 进度回调在生成开始前触发，不是完成后——见 export_tag 里同一处修复
    // 的说明，单张生成慢的时候（尤其是徕卡 DNG，实测半分辨率解码接近
    // 1 秒）完成后才报进度会让第一张的等待期界面上什么都不显示。
    ++done;
    if (on_progress) on_progress(done, static_cast<int>(needs_cache.size()));
    fs::path absolute = fs::path(folder_path) / relative_path;
    auto cache_path = generate_preview_cache(db, project_id, image_id, absolute.string());
    if (cache_path) {
      sqlite3_reset(update_cache_stmt.get());
      sqlite3_bind_text(update_cache_stmt.get(), 1, cache_path->c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(update_cache_stmt.get(), 2, image_id);
      sqlite3_step(update_cache_stmt.get());
    }
  }

  return Result<ProjectId, CreateProjectError>::Ok(project_id);
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
  // images/tags/image_tags 已经靠 ON DELETE CASCADE 清掉了；RAW 预览缓存
  // 是磁盘文件，数据库外键管不到，这里手动清掉整个项目的缓存子目录，避
  // 免留下孤儿文件。缓存目录本来就在 PZT 自己的数据目录下，不是"用户原始
  // 文件"，删除不违反 delete_project 一贯"不触碰磁盘上的原始图片"的承诺。
  std::error_code ec;
  fs::remove_all(raw_preview_cache_dir(db, id), ec);
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
            "SELECT id, project_id, file_path, file_name, file_size, kind, preview_cache_path, "
            "captured_at FROM images WHERE id = ?;");
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
    info.preview_cache_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
  }
  if (sqlite3_column_type(stmt.get(), 7) != SQLITE_NULL) {
    info.captured_at = sqlite3_column_int64(stmt.get(), 7);
  }
  return info;
}

Result<RescanSummary, ProjectNotFoundError> rescan_project(db::Database& db, ProjectId id,
                                                             bool prune,
                                                             ScanProgressFn on_progress) {
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
  std::int64_t upgraded = 0;
  std::vector<std::pair<ImageId, std::string>> needs_cache;  // (image_id, relative_path)

  exec_simple(conn, "BEGIN;");
  try {
    Stmt find_by_path_stmt(
        conn, "SELECT id, captured_at FROM images WHERE project_id = ? AND file_path = ?;");
    Stmt find_by_stem_jpeg_stmt(
        conn,
        "SELECT id FROM images WHERE project_id = ? AND kind = 'jpeg' AND "
        "file_path = ?;");
    Stmt insert_stmt(conn,
                      "INSERT INTO images (project_id, file_path, file_name, file_size, "
                      "imported_at, kind, captured_at) VALUES (?, ?, ?, ?, ?, ?, ?);");
    // 把一条原来是 kind='jpeg' 的记录原地升级成 kind='raw'：file_path/
    // file_name 换成 RAW 的，不插入新记录——否则原来那条 JPEG 记录会在
    // prune 阶段被误判成"磁盘上已消失"，连带丢失已经打过的标签/recipe。
    Stmt upgrade_stmt(
        conn,
        "UPDATE images SET file_path = ?, file_name = ?, kind = 'raw', captured_at = ? "
        "WHERE id = ?;");
    Stmt backfill_captured_at_stmt(conn, "UPDATE images SET captured_at = ? WHERE id = ?;");

    for (const auto& img : scanned) {
      sqlite3_reset(find_by_path_stmt.get());
      sqlite3_bind_int64(find_by_path_stmt.get(), 1, id);
      sqlite3_bind_text(find_by_path_stmt.get(), 2, img.relative_path.c_str(), -1,
                         SQLITE_TRANSIENT);
      if (sqlite3_step(find_by_path_stmt.get()) == SQLITE_ROW) {
        // 已经在 images 表里了。captured_at 还没有的话顺手补上——这样这个
        // 字段上线之前建好的老项目不用删了重建，下一次 rescan 自然补齐。
        // 已经有值的不重新读一遍，rescan 常态下这些行占大多数，不为它们
        // 重复付读取 metadata 的开销。
        if (sqlite3_column_type(find_by_path_stmt.get(), 1) == SQLITE_NULL) {
          ImageId existing_id = sqlite3_column_int64(find_by_path_stmt.get(), 0);
          auto captured_at = read_capture_time(
              img.kind, (fs::path(summary->root_path) / img.relative_path).string());
          if (captured_at) {
            sqlite3_reset(backfill_captured_at_stmt.get());
            sqlite3_bind_int64(backfill_captured_at_stmt.get(), 1, *captured_at);
            sqlite3_bind_int64(backfill_captured_at_stmt.get(), 2, existing_id);
            sqlite3_step(backfill_captured_at_stmt.get());
          }
        }
        continue;
      }

      // file_path 没有精确匹配到已有记录。如果这是一张 RAW 图片，检查同一
      // 文件名主干是不是曾经作为纯 JPEG 记录过——命中就原地升级，不是插入。
      if (img.kind == "raw") {
        fs::path stem = fs::path(img.relative_path).parent_path() /
                         fs::path(img.relative_path).stem();
        // JPEG 扩展名不确定是 .jpg 还是 .jpeg，两个都试一次；这里用不到
        // is_jpeg 那个大小写不敏感的通用逻辑，因为 file_path 本身就是磁盘
        // 上真实存在过的路径，只有这两种可能。
        std::optional<ImageId> jpeg_id;
        for (const char* ext : {".jpg", ".jpeg", ".JPG", ".JPEG"}) {
          sqlite3_reset(find_by_stem_jpeg_stmt.get());
          sqlite3_bind_int64(find_by_stem_jpeg_stmt.get(), 1, id);
          std::string candidate = stem.string() + ext;
          sqlite3_bind_text(find_by_stem_jpeg_stmt.get(), 2, candidate.c_str(), -1,
                             SQLITE_TRANSIENT);
          if (sqlite3_step(find_by_stem_jpeg_stmt.get()) == SQLITE_ROW) {
            jpeg_id = sqlite3_column_int64(find_by_stem_jpeg_stmt.get(), 0);
            break;
          }
        }
        if (jpeg_id) {
          // 现在能拿到 RAW 文件本身了，captured_at 顺手用它重新算一遍——
          // 比升级前用 JPEG 算出来的更权威。
          auto captured_at = read_capture_time(
              "raw", (fs::path(summary->root_path) / img.relative_path).string());
          sqlite3_reset(upgrade_stmt.get());
          sqlite3_bind_text(upgrade_stmt.get(), 1, img.relative_path.c_str(), -1,
                             SQLITE_TRANSIENT);
          sqlite3_bind_text(upgrade_stmt.get(), 2, img.file_name.c_str(), -1, SQLITE_TRANSIENT);
          if (captured_at) {
            sqlite3_bind_int64(upgrade_stmt.get(), 3, *captured_at);
          } else {
            sqlite3_bind_null(upgrade_stmt.get(), 3);
          }
          sqlite3_bind_int64(upgrade_stmt.get(), 4, *jpeg_id);
          if (sqlite3_step(upgrade_stmt.get()) != SQLITE_DONE) {
            throw std::runtime_error(std::string("upgrade image failed: ") + sqlite3_errmsg(conn));
          }
          ++upgraded;
          needs_cache.emplace_back(*jpeg_id, img.relative_path);
          continue;
        }
      }

      auto captured_at = read_capture_time(
          img.kind, (fs::path(summary->root_path) / img.relative_path).string());
      sqlite3_reset(insert_stmt.get());
      sqlite3_bind_int64(insert_stmt.get(), 1, id);
      sqlite3_bind_text(insert_stmt.get(), 2, img.relative_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_stmt.get(), 3, img.file_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(insert_stmt.get(), 4, img.file_size);
      sqlite3_bind_int64(insert_stmt.get(), 5, imported_at);
      sqlite3_bind_text(insert_stmt.get(), 6, img.kind.c_str(), -1, SQLITE_TRANSIENT);
      if (captured_at) {
        sqlite3_bind_int64(insert_stmt.get(), 7, *captured_at);
      } else {
        sqlite3_bind_null(insert_stmt.get(), 7);
      }
      if (sqlite3_step(insert_stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string("insert image failed: ") + sqlite3_errmsg(conn));
      }
      ++added;
      if (img.kind == "raw") {
        needs_cache.emplace_back(sqlite3_last_insert_rowid(conn), img.relative_path);
      }
    }

    if (prune) {
      // images 对 image_tags 是 ON DELETE CASCADE（core/db/schema.cpp 已经
      // PRAGMA foreign_keys = ON，delete_project 依赖的是同一个级联行为），
      // 删这一行就会把这张图打过的标签一并带走，不需要额外手写删
      // image_tags 的语句。
      std::vector<std::tuple<std::int64_t, std::string, std::optional<std::string>>> existing;
      Stmt list_stmt(conn, "SELECT id, file_path, preview_cache_path FROM images WHERE project_id = ?;");
      sqlite3_bind_int64(list_stmt.get(), 1, id);
      while (sqlite3_step(list_stmt.get()) == SQLITE_ROW) {
        std::optional<std::string> cache_path;
        if (sqlite3_column_type(list_stmt.get(), 2) != SQLITE_NULL) {
          cache_path = reinterpret_cast<const char*>(sqlite3_column_text(list_stmt.get(), 2));
        }
        existing.emplace_back(sqlite3_column_int64(list_stmt.get(), 0),
                               reinterpret_cast<const char*>(sqlite3_column_text(list_stmt.get(), 1)),
                               std::move(cache_path));
      }

      Stmt delete_stmt(conn, "DELETE FROM images WHERE id = ?;");
      for (const auto& [image_id, file_path, cache_path] : existing) {
        if (scanned_paths.count(file_path) != 0) continue;  // 磁盘上还在
        sqlite3_reset(delete_stmt.get());
        sqlite3_bind_int64(delete_stmt.get(), 1, image_id);
        if (sqlite3_step(delete_stmt.get()) != SQLITE_DONE) {
          throw std::runtime_error(std::string("delete missing image failed: ") +
                                    sqlite3_errmsg(conn));
        }
        if (cache_path) {
          std::error_code ec;
          fs::remove(*cache_path, ec);  // 孤儿缓存文件清理，失败不影响 rescan 本身
        }
        ++removed;
      }
    }

    exec_simple(conn, "COMMIT;");
  } catch (...) {
    exec_simple(conn, "ROLLBACK;");
    throw;
  }

  // 同 create_project：预览缓存生成放在主事务提交之后,进度回调在生成开
  // 始前触发(见 create_project 里同一处修复的说明)。
  Stmt update_cache_stmt(conn, "UPDATE images SET preview_cache_path = ? WHERE id = ?;");
  int done = 0;
  for (const auto& [image_id, relative_path] : needs_cache) {
    ++done;
    if (on_progress) on_progress(done, static_cast<int>(needs_cache.size()));
    fs::path absolute = fs::path(summary->root_path) / relative_path;
    auto cache_path = generate_preview_cache(db, id, image_id, absolute.string());
    if (cache_path) {
      sqlite3_reset(update_cache_stmt.get());
      sqlite3_bind_text(update_cache_stmt.get(), 1, cache_path->c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(update_cache_stmt.get(), 2, image_id);
      sqlite3_step(update_cache_stmt.get());
    }
  }

  RescanSummary result;
  result.added_count = added;
  result.removed_count = removed;
  result.total_count = summary->image_count + added - removed;
  result.upgraded_count = upgraded;
  return Result<RescanSummary, ProjectNotFoundError>::Ok(result);
}

}  // namespace pzt::core::project
