#include "core/db/schema.h"

#include <stdexcept>
#include <string>

namespace pzt::core::db {

namespace {

void exec(sqlite3* conn, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(conn, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string message = err ? err : "unknown sqlite error";
    sqlite3_free(err);
    throw std::runtime_error("schema init failed: " + message);
  }
}

constexpr const char* kCreateProjects = R"sql(
CREATE TABLE IF NOT EXISTS projects (
  id            INTEGER PRIMARY KEY,
  name          TEXT NOT NULL UNIQUE,
  root_path     TEXT NOT NULL,
  created_at    INTEGER NOT NULL,
  archived_at   INTEGER
);
)sql";

constexpr const char* kCreateImages = R"sql(
CREATE TABLE IF NOT EXISTS images (
  id            INTEGER PRIMARY KEY,
  project_id    INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
  file_path     TEXT NOT NULL,
  file_name     TEXT NOT NULL,
  file_size     INTEGER NOT NULL,
  imported_at   INTEGER NOT NULL,
  UNIQUE(project_id, file_path)
);
)sql";

constexpr const char* kCreateTags = R"sql(
CREATE TABLE IF NOT EXISTS tags (
  id            INTEGER PRIMARY KEY,
  project_id    INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
  name          TEXT NOT NULL,
  cap           INTEGER,
  is_ordered    INTEGER NOT NULL DEFAULT 0,
  is_system     INTEGER NOT NULL DEFAULT 0,
  UNIQUE(project_id, name)
);
)sql";

constexpr const char* kCreateImageTags = R"sql(
CREATE TABLE IF NOT EXISTS image_tags (
  image_id    INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
  tag_id      INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  position    INTEGER,
  tagged_at   INTEGER NOT NULL,
  PRIMARY KEY (image_id, tag_id)
);
)sql";

// PRIMARY KEY (image_id, tag_id) 只对"先按 image_id 过滤"的查询(比如
// tags_for_image)有索引可用——image_id 是这个复合键的第一列。反过来"按
// tag_id 过滤"的查询(filter_by_tag、list_tags 算 tagged_count)没有任何
// 索引可用,只能整表扫描 image_tags。increment 6.4.6 真机测试发现 g + 数
// 字筛选有明显卡顿,查出来就是这个——项目标签关联多了之后每次筛选都要扫
// 一遍全表。
constexpr const char* kCreateImageTagsTagIdIndex = R"sql(
CREATE INDEX IF NOT EXISTS idx_image_tags_tag_id ON image_tags(tag_id);
)sql";

}  // namespace

void initialize_schema(sqlite3* conn) {
  exec(conn, "PRAGMA foreign_keys = ON;");
  exec(conn, kCreateProjects);
  exec(conn, kCreateImages);
  exec(conn, kCreateTags);
  exec(conn, kCreateImageTags);
  exec(conn, kCreateImageTagsTagIdIndex);
}

}  // namespace pzt::core::db
