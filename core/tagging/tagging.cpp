#include "core/tagging/tagging.h"

#include <chrono>
#include <stdexcept>

#include "core/db/stmt.h"

namespace pzt::core::tagging {

namespace {

using db::exec_simple;
using db::Stmt;

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool tag_name_exists(sqlite3* conn, ProjectId project_id, const std::string& name) {
  Stmt stmt(conn, "SELECT 1 FROM tags WHERE project_id = ? AND name = ?;");
  sqlite3_bind_int64(stmt.get(), 1, project_id);
  sqlite3_bind_text(stmt.get(), 2, name.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

struct TagInfo {
  ProjectId project_id;
  std::optional<std::int64_t> cap;
  bool is_ordered;
};

std::optional<TagInfo> get_tag(sqlite3* conn, TagId id) {
  Stmt stmt(conn, "SELECT project_id, cap, is_ordered FROM tags WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;

  TagInfo info;
  info.project_id = sqlite3_column_int64(stmt.get(), 0);
  if (sqlite3_column_type(stmt.get(), 1) != SQLITE_NULL) {
    info.cap = sqlite3_column_int64(stmt.get(), 1);
  }
  info.is_ordered = sqlite3_column_int64(stmt.get(), 2) != 0;
  return info;
}

bool is_tagged(sqlite3* conn, ImageId image_id, TagId tag_id) {
  Stmt stmt(conn, "SELECT 1 FROM image_tags WHERE image_id = ? AND tag_id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, image_id);
  sqlite3_bind_int64(stmt.get(), 2, tag_id);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

std::int64_t tagged_count(sqlite3* conn, TagId tag_id) {
  Stmt stmt(conn, "SELECT COUNT(*) FROM image_tags WHERE tag_id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, tag_id);
  sqlite3_step(stmt.get());
  return sqlite3_column_int64(stmt.get(), 0);
}

// 有序标签按 position 升序，无序标签按 tagged_at 升序 - 跟导出模块的排序
// 规则保持一致（见 docs/M0_Eng_Design.md 导出小节）。
std::vector<TaggedImageRef> ordered_entries(sqlite3* conn, TagId tag_id, bool is_ordered) {
  const char* order_by = is_ordered ? "it.position ASC" : "it.tagged_at ASC";
  std::string sql = std::string("SELECT i.id, i.file_name FROM image_tags it "
                                 "JOIN images i ON i.id = it.image_id "
                                 "WHERE it.tag_id = ? ORDER BY ") +
                     order_by + ";";
  Stmt stmt(conn, sql.c_str());
  sqlite3_bind_int64(stmt.get(), 1, tag_id);

  std::vector<TaggedImageRef> out;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    out.push_back(TaggedImageRef{
        sqlite3_column_int64(stmt.get(), 0),
        reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1)),
    });
  }
  return out;
}

std::int64_t next_position(sqlite3* conn, TagId tag_id) {
  Stmt stmt(conn, "SELECT COALESCE(MAX(position), 0) + 1 FROM image_tags WHERE tag_id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, tag_id);
  sqlite3_step(stmt.get());
  return sqlite3_column_int64(stmt.get(), 0);
}

}  // namespace

Result<TagId, CreateTagError> create_tag(db::Database& db, ProjectId project_id,
                                          const std::string& name, std::optional<std::int64_t> cap,
                                          bool is_ordered, bool is_system) {
  sqlite3* conn = db.handle();
  if (tag_name_exists(conn, project_id, name)) {
    return Result<TagId, CreateTagError>::Err(CreateTagError::NameAlreadyExists);
  }

  Stmt stmt(conn,
            "INSERT INTO tags (project_id, name, cap, is_ordered, is_system) "
            "VALUES (?, ?, ?, ?, ?);");
  sqlite3_bind_int64(stmt.get(), 1, project_id);
  sqlite3_bind_text(stmt.get(), 2, name.c_str(), -1, SQLITE_TRANSIENT);
  if (cap.has_value()) {
    sqlite3_bind_int64(stmt.get(), 3, *cap);
  } else {
    sqlite3_bind_null(stmt.get(), 3);
  }
  sqlite3_bind_int(stmt.get(), 4, is_ordered ? 1 : 0);
  sqlite3_bind_int(stmt.get(), 5, is_system ? 1 : 0);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("insert tag failed: ") + sqlite3_errmsg(conn));
  }
  return Result<TagId, CreateTagError>::Ok(sqlite3_last_insert_rowid(conn));
}

std::vector<TagSummary> list_tags(db::Database& db, ProjectId project_id) {
  Stmt stmt(db.handle(),
            "SELECT t.id, t.name, t.cap, t.is_ordered, t.is_system, COUNT(it.image_id) "
            "FROM tags t LEFT JOIN image_tags it ON it.tag_id = t.id "
            "WHERE t.project_id = ? "
            "GROUP BY t.id ORDER BY t.name ASC;");
  sqlite3_bind_int64(stmt.get(), 1, project_id);

  std::vector<TagSummary> out;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    TagSummary s;
    s.id = sqlite3_column_int64(stmt.get(), 0);
    s.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    if (sqlite3_column_type(stmt.get(), 2) != SQLITE_NULL) {
      s.cap = sqlite3_column_int64(stmt.get(), 2);
    }
    s.is_ordered = sqlite3_column_int64(stmt.get(), 3) != 0;
    s.is_system = sqlite3_column_int64(stmt.get(), 4) != 0;
    s.tagged_count = sqlite3_column_int64(stmt.get(), 5);
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<TagSummary> tags_for_image(db::Database& db, ImageId image_id) {
  Stmt stmt(db.handle(),
            "SELECT t.id, t.name, t.cap, t.is_ordered, t.is_system, "
            "(SELECT COUNT(*) FROM image_tags WHERE tag_id = t.id) "
            "FROM tags t JOIN image_tags it ON it.tag_id = t.id "
            "WHERE it.image_id = ? "
            "ORDER BY t.name ASC;");
  sqlite3_bind_int64(stmt.get(), 1, image_id);

  std::vector<TagSummary> out;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    TagSummary s;
    s.id = sqlite3_column_int64(stmt.get(), 0);
    s.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    if (sqlite3_column_type(stmt.get(), 2) != SQLITE_NULL) {
      s.cap = sqlite3_column_int64(stmt.get(), 2);
    }
    s.is_ordered = sqlite3_column_int64(stmt.get(), 3) != 0;
    s.is_system = sqlite3_column_int64(stmt.get(), 4) != 0;
    s.tagged_count = sqlite3_column_int64(stmt.get(), 5);
    out.push_back(std::move(s));
  }
  return out;
}

std::optional<TagId> find_tag_by_name(db::Database& db, ProjectId project_id,
                                       const std::string& name) {
  Stmt stmt(db.handle(), "SELECT id FROM tags WHERE project_id = ? AND name = ?;");
  sqlite3_bind_int64(stmt.get(), 1, project_id);
  sqlite3_bind_text(stmt.get(), 2, name.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
  return sqlite3_column_int64(stmt.get(), 0);
}

Result<void, AddTagError> add_tag(db::Database& db, ImageId image_id, TagId tag_id) {
  sqlite3* conn = db.handle();

  auto tag = get_tag(conn, tag_id);
  if (!tag) {
    return Result<void, AddTagError>::Err(AddTagError{AddTagFailureKind::TagNotFound, std::nullopt});
  }
  auto image = project::get_image(db, image_id);
  if (!image) {
    return Result<void, AddTagError>::Err(
        AddTagError{AddTagFailureKind::ImageNotFound, std::nullopt});
  }
  if (image->project_id != tag->project_id) {
    return Result<void, AddTagError>::Err(
        AddTagError{AddTagFailureKind::ProjectMismatch, std::nullopt});
  }

  if (is_tagged(conn, image_id, tag_id)) {
    return Result<void, AddTagError>::Ok();  // 幂等
  }

  if (tag->cap.has_value() && tagged_count(conn, tag_id) >= *tag->cap) {
    CapExceededInfo info;
    info.cap = *tag->cap;
    info.existing_entries = ordered_entries(conn, tag_id, tag->is_ordered);
    return Result<void, AddTagError>::Err(
        AddTagError{AddTagFailureKind::CapExceeded, std::move(info)});
  }

  Stmt stmt(conn,
            "INSERT INTO image_tags (image_id, tag_id, position, tagged_at) VALUES (?, ?, ?, ?);");
  sqlite3_bind_int64(stmt.get(), 1, image_id);
  sqlite3_bind_int64(stmt.get(), 2, tag_id);
  if (tag->is_ordered) {
    sqlite3_bind_int64(stmt.get(), 3, next_position(conn, tag_id));
  } else {
    sqlite3_bind_null(stmt.get(), 3);
  }
  sqlite3_bind_int64(stmt.get(), 4, now_unix());
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("insert image_tags failed: ") + sqlite3_errmsg(conn));
  }
  return Result<void, AddTagError>::Ok();
}

Result<void, RemoveTagError> remove_tag(db::Database& db, ImageId image_id, TagId tag_id) {
  sqlite3* conn = db.handle();

  if (!get_tag(conn, tag_id)) {
    return Result<void, RemoveTagError>::Err(RemoveTagError::TagNotFound);
  }
  if (!project::get_image(db, image_id)) {
    return Result<void, RemoveTagError>::Err(RemoveTagError::ImageNotFound);
  }

  Stmt stmt(conn, "DELETE FROM image_tags WHERE image_id = ? AND tag_id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, image_id);
  sqlite3_bind_int64(stmt.get(), 2, tag_id);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("delete image_tags failed: ") + sqlite3_errmsg(conn));
  }
  return Result<void, RemoveTagError>::Ok();  // 幂等，本来就没打标签也算成功
}

Result<void, ReplaceTagError> replace_tag_entry(db::Database& db, TagId tag_id, ImageId old_image,
                                                 ImageId new_image) {
  sqlite3* conn = db.handle();

  auto tag = get_tag(conn, tag_id);
  if (!tag) {
    return Result<void, ReplaceTagError>::Err(ReplaceTagError::TagNotFound);
  }

  std::optional<std::int64_t> old_position;
  {
    Stmt stmt(conn, "SELECT position FROM image_tags WHERE tag_id = ? AND image_id = ?;");
    sqlite3_bind_int64(stmt.get(), 1, tag_id);
    sqlite3_bind_int64(stmt.get(), 2, old_image);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
      return Result<void, ReplaceTagError>::Err(ReplaceTagError::OldImageNotTagged);
    }
    if (sqlite3_column_type(stmt.get(), 0) != SQLITE_NULL) {
      old_position = sqlite3_column_int64(stmt.get(), 0);
    }
  }

  auto new_image_info = project::get_image(db, new_image);
  if (!new_image_info || new_image_info->project_id != tag->project_id) {
    return Result<void, ReplaceTagError>::Err(ReplaceTagError::NewImageNotFound);
  }

  exec_simple(conn, "BEGIN;");
  try {
    {
      Stmt del(conn, "DELETE FROM image_tags WHERE tag_id = ? AND image_id = ?;");
      sqlite3_bind_int64(del.get(), 1, tag_id);
      sqlite3_bind_int64(del.get(), 2, old_image);
      if (sqlite3_step(del.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string("delete old image_tags failed: ") +
                                  sqlite3_errmsg(conn));
      }
    }
    {
      Stmt ins(conn,
               "INSERT INTO image_tags (image_id, tag_id, position, tagged_at) "
               "VALUES (?, ?, ?, ?);");
      sqlite3_bind_int64(ins.get(), 1, new_image);
      sqlite3_bind_int64(ins.get(), 2, tag_id);
      if (old_position.has_value()) {
        sqlite3_bind_int64(ins.get(), 3, *old_position);
      } else {
        sqlite3_bind_null(ins.get(), 3);
      }
      sqlite3_bind_int64(ins.get(), 4, now_unix());
      if (sqlite3_step(ins.get()) != SQLITE_DONE) {
        throw std::runtime_error(std::string("insert new image_tags failed: ") +
                                  sqlite3_errmsg(conn));
      }
    }
    exec_simple(conn, "COMMIT;");
  } catch (...) {
    exec_simple(conn, "ROLLBACK;");
    throw;
  }

  return Result<void, ReplaceTagError>::Ok();
}

Result<void, DeleteTagError> delete_tag(db::Database& db, TagId tag_id) {
  sqlite3* conn = db.handle();

  bool is_system = false;
  {
    Stmt stmt(conn, "SELECT is_system FROM tags WHERE id = ?;");
    sqlite3_bind_int64(stmt.get(), 1, tag_id);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
      return Result<void, DeleteTagError>::Err(DeleteTagError::TagNotFound);
    }
    is_system = sqlite3_column_int64(stmt.get(), 0) != 0;
  }
  if (is_system) {
    return Result<void, DeleteTagError>::Err(DeleteTagError::SystemTagProtected);
  }

  Stmt del(conn, "DELETE FROM tags WHERE id = ?;");
  sqlite3_bind_int64(del.get(), 1, tag_id);
  if (sqlite3_step(del.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("delete tag failed: ") + sqlite3_errmsg(conn));
  }
  return Result<void, DeleteTagError>::Ok();
}

TagId ensure_reject_tag(db::Database& db, ProjectId project_id) {
  auto existing = find_tag_by_name(db, project_id, kRejectTagName);
  if (existing) return *existing;

  // find-then-create 之间理论上有 TOCTOU 窗口，但调用点（pzt new/pzt open
  // 启动阶段）都是单线程、单进程的，不是并发场景，不需要额外加锁处理。
  auto created =
      create_tag(db, project_id, kRejectTagName, std::nullopt, /*is_ordered=*/false,
                 /*is_system=*/true);
  return created.value();
}

TagId ensure_duplicate_tag(db::Database& db, ProjectId project_id) {
  auto existing = find_tag_by_name(db, project_id, kDuplicateTagName);
  if (existing) return *existing;

  auto created =
      create_tag(db, project_id, kDuplicateTagName, std::nullopt, /*is_ordered=*/false,
                 /*is_system=*/true);
  return created.value();
}

}  // namespace pzt::core::tagging
