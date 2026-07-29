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
// 有意义，deleted_at 是软删除标记）。见 docs/history/M1_Eng_Design.md "数据库
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
// docs/history/M3_Eng_Design.md"数据库 Schema 设计"一节。跟 images 表分开建一张
// 表而不是继续加列——上一版只有 4 列时挤在 images 上还说得过去，这次三
// 个维度各自的分数/原因/修正建议加起来十几列，继续堆在 images 上会把
// "文件本身的元数据"和"AI 评估结果"这两个不同职责混在一起。image_id 直
// 接当主键(一对一关系，不单独设自增 id)，ON DELETE CASCADE 跟
// tags/image_tags 现有的级联删除惯例一致。这张表要么整行存在(评估完整
// 成功)要么整行不存在(没评估过/评估失败)，不是"整行都在、单个字段可
// 空"的语义——所以除了两个修正建议各自四五个字段允许 NULL(模型判断不
// 需要修正建议时不给)之外，其它列都是 NOT NULL。
// W2026-07-21：eval 从三维技术打分改成"一段客观文字 assessment + 一个
// unusable 硬伤 flag"，这张表整体重建成 5 列，随后又把 assessment/
// unusable 这两列合并成一列 result_json(存模型原始返回的
// {"assessment":..,"unusable":..})——理由是这两列都是"问 AI 要的值"，以后
// 想再加一个类似的值(比如再问一个维度)不该每次都要一次破坏性表重建；
// extra_guidance/provider 不是模型输出、是调用方自己知道的上下文，仍然
// 留作独立列。image_id 当主键(一对一)，ON DELETE CASCADE 跟
// tags/image_tags 惯例一致。整行要么存在(评估完整成功)要么不存在，所以
// 除主键外都 NOT NULL。旧 schema(三维打分列、或上一版的 assessment+
// unusable 列)由 initialize_schema 里的一次性 DROP TABLE 迁移清掉(见那
// 里)。
constexpr const char* kCreateImageEvaluations = R"sql(
CREATE TABLE IF NOT EXISTS image_evaluations (
  image_id        INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
  result_json     TEXT NOT NULL,
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

// column_exists 在"表不存在"和"表存在但没这一列"两种情况下都返回 false。
// 版本 0 的迁移要靠"表在不在"来区分"全新安装"和"老库结构过时",必须能分
// 开这两件事,所以单独查 sqlite_master(这里可以用 bind,不像 PRAGMA)。
bool table_exists(sqlite3* conn, const char* table) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1,
                          &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("table_exists: failed to prepare sqlite_master query");
  }
  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  bool found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

int read_user_version(sqlite3* conn) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("read_user_version: failed to prepare PRAGMA user_version");
  }
  int version = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return version;
}

// PRAGMA 不接受 bind 参数,只能拼串——跟 ensure_column 拼 ALTER TABLE 是
// 一样的处理,值是本文件里的常量,不来自外部输入。
void set_user_version(sqlite3* conn, int version) {
  std::string sql = "PRAGMA user_version = " + std::to_string(version) + ";";
  exec(conn, sql.c_str());
}

// 版本 0 到 1 的一次性迁移。0 同时覆盖了全新空文件和任何 T-7 之前建的老
// 库:每条建表都是 CREATE TABLE IF NOT EXISTS、每条加列都先查列在不在,所
// 以两者走同一段代码,不需要区分"新装用户"和"升级用户"。
//
// 整段套一个事务:SQLite 的 DDL 是事务性的,PRAGMA user_version 的写入也
// 走同一份 journal,于是"迁移完 + 盖章"变成原子的,中途崩溃不会留下一个
// 结构改了一半却已经盖章的库。盖章仍然放在最后,所以即使事务机制本身出
// 意外,版本也只会停在 0,下次开库原样重跑。
//
// BEGIN IMMEDIATE 而不是默认的 deferred:agent 每张图起一个 pzt 进程,首
// 次升级时多个进程同时开库是真实场景。IMMEDIATE 让它们在写锁上排队(靠
// Database::open_at 设的 5 秒 busy_timeout 等),deferred 则会在中途升写
// 锁时撞 SQLITE_BUSY,更糟的是存在真实交错:B 的 CREATE TABLE IF NOT
// EXISTS image_evaluations 落在 A 的结构校验与 A 的 DROP TABLE 之间,结
// 果表被删掉。
void migrate_v0_to_v1(sqlite3* conn) {
  exec(conn, "BEGIN IMMEDIATE;");
  try {
    exec(conn, kCreateProjects);
    exec(conn, kCreateImages);
    exec(conn, kCreateTags);
    exec(conn, kCreateImageTags);
    exec(conn, kCreateImageTagsTagIdIndex);
    exec(conn, kCreateRecipes);
    exec(conn, kCreateRecipesPresetNameIndex);
    // W2026-07-21 那两次 eval schema 重建(三维打分列 → assessment+unusable
    // 两列 → 合并成一列 result_json)都是整表 drop 重建、不写迁移,依据是
    // "库里都是迭代测试数据,无真实用户数据要保留(PRD 已拍板)"。这个前提
    // 在 Homebrew tap 分发上线之后不再成立,而原来的实现是按列名匹配
    // (存在 exposure_score 或 unusable 就 drop)、且常驻在每次开库的路径
    // 上,既会误伤未来出现的同名列,也让一次破坏性操作永远挂在热路径上。
    //
    // 现在改成按结构判定,并且只在这条 v0→v1 迁移里跑一次:表在、但没有
    // result_json 列,说明它是 W2026-07-21 之前的形态,存的东西跟现在的读
    // 法对不上,drop 掉重建。表不存在(全新安装)或已经有 result_json(跑过
    // 现代版本的库)都不动,评估结果原样保留。
    if (table_exists(conn, "image_evaluations") &&
        !column_exists(conn, "image_evaluations", "result_json")) {
      exec(conn, "DROP TABLE image_evaluations;");
    }
    exec(conn, kCreateImageEvaluations);
    ensure_column(conn, "images", "recipe_id",
                  "recipe_id INTEGER REFERENCES recipes(id) ON DELETE SET NULL");
    // M2: 图片来源类型 + RAW 预览缓存路径。见 docs/history/M2_Eng_Design.md"数据库
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
    // dropped 辅助函数）。T-7 把同一条推理推广成了版本闸门。
    // 目标二：预设级烘焙好的颗粒强度(0..1)，跟 base_lut/base_lut_size 一样
    // 是"预设的底子"，version 不能覆盖。默认值 0 让旧库迁移时所有已有预设
    // (包括即将被清理的占位 Warm)行为不变，见
    // docs/history/W2026-07-15_RecipeExpansion_Eng_Design.md。
    ensure_column(conn, "recipes", "grain_amount", "grain_amount REAL NOT NULL DEFAULT 0");
    // 目标二第二刀：用户自建 version 新增的四个可调旋钮，见
    // docs/history/W2026-07-15_RecipeExpansion_Eng_Design.md 第八节。默认值 0 让旧
    // 库里已有的 version 行(比如"亮一点"、"test1")迁移后这四个新旋钮自动
    // 落在中性状态，不影响现有效果。
    ensure_column(conn, "recipes", "contrast", "contrast REAL NOT NULL DEFAULT 0");
    ensure_column(conn, "recipes", "saturation", "saturation REAL NOT NULL DEFAULT 0");
    ensure_column(conn, "recipes", "blacks", "blacks REAL NOT NULL DEFAULT 0");
    ensure_column(conn, "recipes", "whites", "whites REAL NOT NULL DEFAULT 0");
    set_user_version(conn, kSchemaVersion);
    exec(conn, "COMMIT;");
  } catch (...) {
    // 裸 sqlite3_exec 而不是 exec:回滚本身失败时不能再抛,否则会掩盖真正
    // 的原始错误。
    sqlite3_exec(conn, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
}

}  // namespace

SchemaTooNewError::SchemaTooNewError(int found, int supported)
    : std::runtime_error("database schema version " + std::to_string(found) +
                          " is newer than supported version " + std::to_string(supported)),
      found_(found),
      supported_(supported) {}

void initialize_schema(sqlite3* conn) {
  // foreign_keys 是每条连接各自的设置,不随库文件走,所以它在闸门之外、
  // 每次开库都必须跑。它也必须在事务之外:事务活跃时 SQLite 会静默忽略
  // 这条 pragma。
  exec(conn, "PRAGMA foreign_keys = ON;");

  const int version = read_user_version(conn);

  // 已经是当前版本:库的结构由版本号自己声明是完整的,不再跑 7 条建表和
  // 11 次 PRAGMA table_info 去重复确认一个已知的答案。这是版本闸门的应
  // 有收益,也是它的代价,见 kSchemaVersion 上关于"改 schema 必须 bump"
  // 的说明。(同样的推理见下面那段 F-33 注释:检查一个不可能再发生的迁
  // 移是纯粹的浪费。)
  if (version == kSchemaVersion) return;

  if (version > kSchemaVersion) throw SchemaTooNewError(version, kSchemaVersion);

  migrate_v0_to_v1(conn);
}

}  // namespace pzt::core::db
