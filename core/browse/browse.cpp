#include "core/browse/browse.h"

#include <algorithm>

#include "core/db/stmt.h"

namespace pzt::core::browse {

namespace {

using db::Stmt;

bool has_any_tag(sqlite3* conn, ImageId image_id) {
  Stmt stmt(conn, "SELECT 1 FROM image_tags WHERE image_id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, image_id);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

// nullopt 表示标签不存在。
std::optional<bool> tag_is_ordered(sqlite3* conn, TagId tag_id) {
  Stmt stmt(conn, "SELECT is_ordered FROM tags WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, tag_id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
  return sqlite3_column_int64(stmt.get(), 0) != 0;
}

std::optional<std::size_t> index_of(const std::vector<ImageRef>& images,
                                     std::optional<ImageId> id) {
  if (!id) return std::nullopt;
  auto it = std::find_if(images.begin(), images.end(),
                          [&](const ImageRef& r) { return r.id == *id; });
  if (it == images.end()) return std::nullopt;  // 不在列表里，按 nullopt 防御性处理
  return static_cast<std::size_t>(std::distance(images.begin(), it));
}

// list_images/filter_by_tag 的 SELECT 都按同样的 5 列顺序取(id, file_path,
// file_name, kind, preview_cache_path)，抽出来避免重复。
ImageRef row_to_image_ref(sqlite3_stmt* stmt) {
  ImageRef ref;
  ref.id = sqlite3_column_int64(stmt, 0);
  ref.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  ref.file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  ref.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
    ref.preview_cache_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  }
  return ref;
}

}  // namespace

std::vector<ImageRef> list_images(db::Database& db, ProjectId project_id) {
  // M2 收尾：按拍摄时间排序，取代纯文件名排序——多相机场景下不同机身的
  // 文件名计数器互相独立，按文件名交替排列跟实际拍摄顺序没关系。
  // `captured_at IS NULL` 对有值的行是 0、没值的行是 1，ORDER BY 这一列
  // 自然把有值的排在前面；有值的再按 captured_at 升序排，没值的落到最
  // 后按 file_path 兜底。
  Stmt stmt(db.handle(),
            "SELECT id, file_path, file_name, kind, preview_cache_path FROM images "
            "WHERE project_id = ? "
            "ORDER BY captured_at IS NULL, captured_at ASC, file_path ASC;");
  sqlite3_bind_int64(stmt.get(), 1, project_id);

  std::vector<ImageRef> out;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    out.push_back(row_to_image_ref(stmt.get()));
  }
  return out;
}

std::optional<ImageId> next_image(const std::vector<ImageRef>& images,
                                   std::optional<ImageId> current_id) {
  if (images.empty()) return std::nullopt;
  auto idx = index_of(images, current_id);
  if (!idx) return images.front().id;  // 没选定，或者选定的不在列表里 -> 从头开始
  return images[(*idx + 1) % images.size()].id;
}

std::optional<ImageId> prev_image(const std::vector<ImageRef>& images,
                                   std::optional<ImageId> current_id) {
  if (images.empty()) return std::nullopt;
  auto idx = index_of(images, current_id);
  if (!idx) return images.back().id;  // 没选定时，"上一张"循环语义上是最后一张
  return images[(*idx + images.size() - 1) % images.size()].id;
}

std::optional<ImageId> next_untagged(db::Database& db, const std::vector<ImageRef>& images,
                                      std::optional<ImageId> current_id) {
  if (images.empty()) return std::nullopt;
  std::optional<ImageId> candidate = current_id;
  for (std::size_t i = 0; i < images.size(); ++i) {
    candidate = next_image(images, candidate);
    if (candidate && !has_any_tag(db.handle(), *candidate)) return candidate;
  }
  return std::nullopt;  // 转了一整圈都没有未打标签的图片
}

std::optional<ImageId> prev_untagged(db::Database& db, const std::vector<ImageRef>& images,
                                      std::optional<ImageId> current_id) {
  if (images.empty()) return std::nullopt;
  std::optional<ImageId> candidate = current_id;
  for (std::size_t i = 0; i < images.size(); ++i) {
    candidate = prev_image(images, candidate);
    if (candidate && !has_any_tag(db.handle(), *candidate)) return candidate;
  }
  return std::nullopt;
}

Result<std::vector<ImageRef>, BrowseTagError> filter_by_tag(db::Database& db, TagId tag_id) {
  sqlite3* conn = db.handle();
  auto is_ordered = tag_is_ordered(conn, tag_id);
  if (!is_ordered) {
    return Result<std::vector<ImageRef>, BrowseTagError>::Err(BrowseTagError::TagNotFound);
  }

  const char* order_by = *is_ordered ? "it.position ASC" : "it.tagged_at ASC";
  std::string sql =
      std::string("SELECT i.id, i.file_path, i.file_name, i.kind, i.preview_cache_path "
                   "FROM image_tags it "
                   "JOIN images i ON i.id = it.image_id "
                   "WHERE it.tag_id = ? ORDER BY ") +
      order_by + ";";
  Stmt stmt(conn, sql.c_str());
  sqlite3_bind_int64(stmt.get(), 1, tag_id);

  std::vector<ImageRef> out;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    out.push_back(row_to_image_ref(stmt.get()));
  }
  return Result<std::vector<ImageRef>, BrowseTagError>::Ok(std::move(out));
}

}  // namespace pzt::core::browse
