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

// M3 增量一修订版：选片辅助评估（曝光/构图/对焦），见
// docs/M3_Eng_Design.md"数据库 Schema 设计"一节。跟 images 表分开建一张
// 表而不是继续加列——上一版只有 4 列时挤在 images 上还说得过去，这次三
// 个维度各自的分数/原因/修正建议加起来十几列，继续堆在 images 上会把
// "文件本身的元数据"和"AI 评估结果"这两个不同职责混在一起。image_id 直
// 接当主键(一对一关系，不单独设自增 id)，ON DELETE CASCADE 跟
// tags/image_tags 现有的级联删除惯例一致。这张表要么整行存在(评估完整
// 成功)要么整行不存在(没评估过/评估失败)，不是"整行都在、单个字段可
// 空"的语义——所以除了两个修正建议各自四五个字段允许 NULL(模型判断不
// 需要修正建议时不给)之外，其它列都是 NOT NULL。
// W2026-07-21：eval 从三维技术打分改成"一段客观文字 assessment + 一个
// unusable 硬伤 flag"，这张表整体重建成 5 列。image_id 当主键(一对一)，
// ON DELETE CASCADE 跟 tags/image_tags 惯例一致。整行要么存在(评估完整成
// 功)要么不存在，所以除主键外都 NOT NULL。旧的三维 schema 由
// initialize_schema 里的一次性 DROP TABLE 迁移清掉(见那里)。
constexpr const char* kCreateImageEvaluations = R"sql(
CREATE TABLE IF NOT EXISTS image_evaluations (
  image_id        INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
  assessment      TEXT NOT NULL,
  unusable        INTEGER NOT NULL,
  extra_guidance  TEXT NOT NULL,
  provider        TEXT NOT NULL
);
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
  // W2026-07-21：eval schema 从三维打分整体重建成 assessment+unusable。旧
  // 表检测到还带 exposure_score 列时整表 drop——库里都是迭代测试数据，无
  // 真实用户数据要保留，直接作废重设不写迁移(PRD 已拍板)。幂等：重建后
  // exposure_score 不存在，后续开库不再 drop。DROP 必须在 CREATE 之前。
  if (column_exists(conn, "image_evaluations", "exposure_score")) {
    exec(conn, "DROP TABLE image_evaluations;");
  }
  exec(conn, kCreateImageEvaluations);
  ensure_column(conn, "images", "recipe_id",
                "recipe_id INTEGER REFERENCES recipes(id) ON DELETE SET NULL");
  // M2: 图片来源类型 + RAW 预览缓存路径。见 docs/M2_Eng_Design.md"数据库
  // Schema 设计"。默认值 'jpeg' 让 M0/M1 时代建的旧库迁移时所有已有行行为
  // 不变，不需要区分新装/升级用户。kind 只有 'jpeg'/'raw' 两态——同名 JPEG
  // 存在时直接忽略，不做配对。
  ensure_column(conn, "images", "kind", "kind TEXT NOT NULL DEFAULT 'jpeg'");
  ensure_column(conn, "images", "preview_cache_path", "preview_cache_path TEXT");
  // M2 收尾：拍摄时间(Unix 秒数，从 EXIF/LibRaw 提取)，用来把 list_images
  // 的默认浏览顺序从"按文件名"换成"按拍摄时间"——多相机场景下文件名交替
  // 跟实际拍摄顺序没关系。可空:相机没提供、文件读取失败都落在 NULL，
  // list_images 按"NULL 排最后、用文件名兜底"处理，不是错误状态。旧库
  // 迁移时全部落在 NULL，下一次 rescan 会顺手回填。
  ensure_column(conn, "images", "captured_at", "captured_at INTEGER");
  // RAW 支持默认关闭、opt-in（`pzt new`/`pzt rescan` 传 `--support-raw` 才
  // 会读取和处理 RAW 文件）。见 docs/RAW_Support.md。旧库迁移时所有项目落
  // 在 0（未开启），跟 M0/M1 时代"没有 RAW 概念"的项目语义一致。一旦被
  // 打开过就不会自动关闭，没有对应的取消开关。
  ensure_column(conn, "projects", "support_raw", "support_raw INTEGER NOT NULL DEFAULT 0");
  // F-24 会话续点：记住每个项目上次浏览到的那张图,重开时回到那里。可空整
  // 数,旧库迁移落 NULL(等同"无续点")。不加外键约束,靠打开时"该 id 是否
  // 还在图片列表里"的成员检查兜住图被删/prune 掉的情况(见 cmd_open)。
  ensure_column(conn, "projects", "last_image_id", "last_image_id INTEGER");
  // F-33（曾经在这里）：M3 增量一修订把"审美评分"用的四个旧列（1-100
  // 综合分+点评）换成了上面的 image_evaluations 表，当时加了
  // ensure_column_dropped 在每次开库时把旧列清掉。那批列上的数据只是
  // 开发过程里的测试数据，从来没有真实用户数据要保护——这是一个单用户
  // 个人工具，唯一的真实数据库(~/.config/pzt/pzt.db)早就在那次改动之
  // 后打开过、迁移已经跑完，旧列已确认不存在。继续每次开库都跑 4 次
  // PRAGMA table_info 检查一个已经不可能再发生的迁移是纯粹的浪费，删
  // 掉这个一次性清理逻辑（连同已经没有其它调用方的 ensure_column_
  // dropped 辅助函数）。
  // 目标二：预设级烘焙好的颗粒强度(0..1)，跟 base_lut/base_lut_size 一样
  // 是"预设的底子"，version 不能覆盖。默认值 0 让旧库迁移时所有已有预设
  // (包括即将被清理的占位 Warm)行为不变，见
  // docs/W2026-07-15_RecipeExpansion_Eng_Design.md。
  ensure_column(conn, "recipes", "grain_amount", "grain_amount REAL NOT NULL DEFAULT 0");
  // 目标二第二刀：用户自建 version 新增的四个可调旋钮，见
  // docs/W2026-07-15_RecipeExpansion_Eng_Design.md 第八节。默认值 0 让旧
  // 库里已有的 version 行(比如"亮一点"、"test1")迁移后这四个新旋钮自动
  // 落在中性状态，不影响现有效果。
  ensure_column(conn, "recipes", "contrast", "contrast REAL NOT NULL DEFAULT 0");
  ensure_column(conn, "recipes", "saturation", "saturation REAL NOT NULL DEFAULT 0");
  ensure_column(conn, "recipes", "blacks", "blacks REAL NOT NULL DEFAULT 0");
  ensure_column(conn, "recipes", "whites", "whites REAL NOT NULL DEFAULT 0");
}

}  // namespace pzt::core::db
