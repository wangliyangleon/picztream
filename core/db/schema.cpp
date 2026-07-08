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

// M1: 两层 recipe 模型（预设/version）用同一张自引用表表达，parent_id 为
// NULL 的行是预设（is_system 恒为 1，base_lut/base_lut_size 有意义），非
// NULL 的行是某个预设下用户保存的 version（highlights/shadows/wb_shift_*
// 有意义，deleted_at 是软删除标记）。见 docs/M1_Eng_Design.md "数据库
// Schema 设计"。
constexpr const char* kCreateRecipes = R"sql(
CREATE TABLE IF NOT EXISTS recipes (
  id             INTEGER PRIMARY KEY,
  parent_id      INTEGER REFERENCES recipes(id) ON DELETE CASCADE,
  name           TEXT,
  is_system      INTEGER NOT NULL DEFAULT 0,
  base_lut_size  INTEGER,
  base_lut       BLOB,
  highlights     REAL NOT NULL DEFAULT 0,
  shadows        REAL NOT NULL DEFAULT 0,
  wb_shift_r     REAL NOT NULL DEFAULT 0,
  wb_shift_b     REAL NOT NULL DEFAULT 0,
  created_at     INTEGER NOT NULL,
  deleted_at     INTEGER
);
)sql";

// 局部唯一索引只约束预设(parent_id IS NULL)的名字不重复；version 的名字
// 允许 NULL/重复，一个覆盖全表的 UNIQUE 会错误地阻止不同预设下出现同名
// version。
constexpr const char* kCreateRecipesPresetNameIndex = R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS idx_recipes_preset_name ON recipes(name) WHERE parent_id IS NULL;
)sql";

// 本项目第一次需要处理"给已存在的表加列"——之前 initialize_schema 全是
// 幂等的 CREATE TABLE IF NOT EXISTS，从来没遇到过这种情况。column_exists
// 是幂等性的保证：新库和 M0 时代建的老库都统一走这条路径，不需要区分
// "新装用户"和"从 M0 升级的用户"。
bool column_exists(sqlite3* conn, const char* table, const char* column) {
  std::string sql = std::string("PRAGMA table_info(") + table + ");";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("column_exists: failed to prepare PRAGMA table_info");
  }
  bool found = false;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* name = sqlite3_column_text(stmt, 1);  // table_info 第 2 列是列名
    if (name && std::string(column) == reinterpret_cast<const char*>(name)) {
      found = true;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

void ensure_column(sqlite3* conn, const char* table, const char* column,
                    const char* add_column_ddl) {
  if (column_exists(conn, table, column)) return;
  std::string sql = std::string("ALTER TABLE ") + table + " ADD COLUMN " + add_column_ddl + ";";
  exec(conn, sql.c_str());
}

}  // namespace

void initialize_schema(sqlite3* conn) {
  exec(conn, "PRAGMA foreign_keys = ON;");
  exec(conn, kCreateProjects);
  exec(conn, kCreateImages);
  exec(conn, kCreateTags);
  exec(conn, kCreateImageTags);
  exec(conn, kCreateImageTagsTagIdIndex);
  exec(conn, kCreateRecipes);
  exec(conn, kCreateRecipesPresetNameIndex);
  ensure_column(conn, "images", "recipe_id",
                "recipe_id INTEGER REFERENCES recipes(id) ON DELETE SET NULL");
  // M2: 图片来源类型 + 配对的 RAW 文件路径。见 docs/M2_Eng_Design.md"数据库
  // Schema 设计"。默认值 'jpeg' 让 M0/M1 时代建的旧库迁移时所有已有行行为
  // 不变，不需要区分新装/升级用户。
  ensure_column(conn, "images", "kind", "kind TEXT NOT NULL DEFAULT 'jpeg'");
  ensure_column(conn, "images", "raw_path", "raw_path TEXT");
}

}  // namespace pzt::core::db
