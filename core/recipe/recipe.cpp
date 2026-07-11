#include "core/recipe/recipe.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "core/db/stmt.h"

namespace pzt::core::recipe {

namespace {

using db::Stmt;

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// F-32：ensure_default_presets 在每次 list_presets()/list_versions() 门
// 面调用时都会跑一遍(每次按 `r` 键至少一次)——不检查的话，seed_preset
// 会在真正需要它之前就先把 make_warm_lut(17) 整个算一遍(17³ 格点、每
// 个格点几次 sin() 调用)，算完才被 INSERT OR IGNORE 直接扔掉。先查一
// 遍这个预设名字是不是已经播种过，跳过明显不需要的重新计算。
bool preset_name_exists(sqlite3* conn, const std::string& name) {
  Stmt stmt(conn, "SELECT 1 FROM recipes WHERE name = ? AND parent_id IS NULL;");
  sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

// 随手调的暖色偏移，只用来验证"确实在处理颜色"这条路径，不追求调色质
// 量——公式照抄 spikes/color_lut_probe/probe.cpp 的 make_lut()，真正的预
// 设调色设计是后续可以随时补充的独立工作，不阻塞这次的机制验证。
std::vector<float> make_warm_lut(int n) {
  std::vector<float> lut(static_cast<std::size_t>(n) * n * n * 3);
  for (int r = 0; r < n; ++r) {
    for (int g = 0; g < n; ++g) {
      for (int b = 0; b < n; ++b) {
        float rf = static_cast<float>(r) / (n - 1);
        float gf = static_cast<float>(g) / (n - 1);
        float bf = static_cast<float>(b) / (n - 1);
        float rf2 = std::clamp(rf + 0.08f * std::sin(rf * 3.14159f), 0.f, 1.f);
        float gf2 = std::clamp(gf + 0.02f * std::sin((gf - 0.3f) * 3.14159f), 0.f, 1.f);
        float bf2 = std::clamp(bf - 0.08f * std::sin(bf * 3.14159f), 0.f, 1.f);
        std::size_t idx = (static_cast<std::size_t>(r) * n + g) * n + b;
        lut[idx * 3 + 0] = rf2;
        lut[idx * 3 + 1] = gf2;
        lut[idx * 3 + 2] = bf2;
      }
    }
  }
  return lut;
}

void seed_preset(sqlite3* conn, const std::string& name, int lut_size,
                  const std::vector<float>& lut) {
  Stmt stmt(conn, R"sql(
    INSERT OR IGNORE INTO recipes
      (parent_id, name, is_system, base_lut_size, base_lut, created_at)
    VALUES (NULL, ?, 1, ?, ?, ?);
  )sql");
  sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 2, lut_size);
  sqlite3_bind_blob(stmt.get(), 3, lut.data(), static_cast<int>(lut.size() * sizeof(float)),
                     SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.get(), 4, now_unix());
  // F-17：`INSERT OR IGNORE` 命中已存在的预设名字时也返回 SQLITE_DONE
  // (这是幂等播种的正常情形，不是错误)——只有真正的写入失败(磁盘满、
  // 库损坏)才会拿到别的返回值，跟 project::/tagging:: 现有的"查了就
  // throw"约定统一。
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("seed preset failed: ") + sqlite3_errmsg(conn));
  }
}

// "Origin" 没有 base_lut——它代表"没有基础调色风格，只有亮度/白平衡这
// 类细节可调"，固定用 id=0(照抄"废片"系统标签固定占 0 号位的先例)。跟
// seed_preset 不共用一个函数,因为它的行相比其它预设少了 base_lut_size/
// base_lut 两列,硬塞同一个函数签名反而要传一堆没意义的空值。
void seed_origin_preset(sqlite3* conn) {
  Stmt stmt(conn, R"sql(
    INSERT OR IGNORE INTO recipes (id, parent_id, name, is_system, created_at)
    VALUES (0, NULL, 'Origin', 1, ?);
  )sql");
  sqlite3_bind_int64(stmt.get(), 1, now_unix());
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {  // 同 seed_preset，见上面的说明
    throw std::runtime_error(std::string("seed origin preset failed: ") + sqlite3_errmsg(conn));
  }
}

// increment 2 的三个写操作(create/rename/delete_version)都要先弄清楚一
//个 id 到底是不是一个"活着的" version(存在、且不是预设、且没有被软删
// 除),这个小结构体+查询是这三个函数共用的第一步。
struct RecipeRow {
  std::optional<RecipeId> parent_id;  // 空 = 这一行是预设
  bool deleted;
};

std::optional<RecipeRow> get_recipe_row(sqlite3* conn, RecipeId id) {
  Stmt stmt(conn, "SELECT parent_id, deleted_at FROM recipes WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
  RecipeRow row;
  row.parent_id = sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL
                      ? std::nullopt
                      : std::optional<RecipeId>(sqlite3_column_int64(stmt.get(), 0));
  row.deleted = sqlite3_column_type(stmt.get(), 1) != SQLITE_NULL;
  return row;
}

}  // namespace

std::vector<PresetSummary> list_presets(db::Database& db) {
  Stmt stmt(db.handle(), "SELECT id, name FROM recipes WHERE parent_id IS NULL ORDER BY id ASC;");
  std::vector<PresetSummary> out;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    PresetSummary p;
    p.id = sqlite3_column_int64(stmt.get(), 0);
    const unsigned char* name = sqlite3_column_text(stmt.get(), 1);
    p.name = name ? reinterpret_cast<const char*>(name) : "";
    out.push_back(std::move(p));
  }
  return out;
}

std::vector<VersionSummary> list_versions(db::Database& db, RecipeId preset_id) {
  Stmt stmt(db.handle(),
            "SELECT id, parent_id, name, highlights, shadows, wb_shift_r, wb_shift_b, deleted_at "
            "FROM recipes WHERE parent_id = ? ORDER BY id ASC;");
  sqlite3_bind_int64(stmt.get(), 1, preset_id);
  std::vector<VersionSummary> out;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    VersionSummary v;
    v.id = sqlite3_column_int64(stmt.get(), 0);
    v.preset_id = sqlite3_column_int64(stmt.get(), 1);
    const unsigned char* name = sqlite3_column_text(stmt.get(), 2);
    v.name = name ? std::optional<std::string>(reinterpret_cast<const char*>(name)) : std::nullopt;
    v.highlights = sqlite3_column_double(stmt.get(), 3);
    v.shadows = sqlite3_column_double(stmt.get(), 4);
    v.wb_shift_r = sqlite3_column_double(stmt.get(), 5);
    v.wb_shift_b = sqlite3_column_double(stmt.get(), 6);
    v.deleted = sqlite3_column_type(stmt.get(), 7) != SQLITE_NULL;
    out.push_back(std::move(v));
  }
  return out;
}

void ensure_default_presets(db::Database& db) {
  seed_origin_preset(db.handle());  // 没有 LUT 要算，INSERT OR IGNORE 本身足够便宜，不需要同样的守卫
  if (!preset_name_exists(db.handle(), "Warm")) {
    seed_preset(db.handle(), "Warm", 17, make_warm_lut(17));
  }
}

Result<RecipeId, CreateVersionError> create_version(db::Database& db, RecipeId preset_id,
                                                     std::optional<std::string> name,
                                                     VersionParams params) {
  sqlite3* conn = db.handle();
  auto row = get_recipe_row(conn, preset_id);
  if (!row || row->parent_id.has_value()) {
    // 不存在，或者这个 id 本身是个 version(有 parent_id)——两种情况都不
    // 是一个有效的预设 id，用同一个错误值，调用方不需要区分。
    return Result<RecipeId, CreateVersionError>::Err(CreateVersionError::PresetNotFound);
  }

  Stmt stmt(conn, R"sql(
    INSERT INTO recipes
      (parent_id, name, is_system, highlights, shadows, wb_shift_r, wb_shift_b, created_at)
    VALUES (?, ?, 0, ?, ?, ?, ?, ?);
  )sql");
  sqlite3_bind_int64(stmt.get(), 1, preset_id);
  if (name) {
    sqlite3_bind_text(stmt.get(), 2, name->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt.get(), 2);
  }
  sqlite3_bind_double(stmt.get(), 3, params.highlights);
  sqlite3_bind_double(stmt.get(), 4, params.shadows);
  sqlite3_bind_double(stmt.get(), 5, params.wb_shift_r);
  sqlite3_bind_double(stmt.get(), 6, params.wb_shift_b);
  sqlite3_bind_int64(stmt.get(), 7, now_unix());
  // F-17：以前不检查这一步，插入真失败(磁盘满等)时 sqlite3_last_insert_
  // rowid 会返回上一条无关插入的 rowid，把它当成新建的 version id 交还
  // 给调用方——是一个真实但极少触发的正确性 bug，跟 project::/
  // tagging:: 现有的"查了就 throw"约定统一，不静默吞掉写入失败。
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("insert version failed: ") + sqlite3_errmsg(conn));
  }

  return Result<RecipeId, CreateVersionError>::Ok(sqlite3_last_insert_rowid(conn));
}

Result<void, RecipeOpError> rename_version(db::Database& db, RecipeId version_id,
                                            const std::string& new_name) {
  sqlite3* conn = db.handle();
  auto row = get_recipe_row(conn, version_id);
  if (!row || row->deleted) return Result<void, RecipeOpError>::Err(RecipeOpError::NotFound);
  if (!row->parent_id.has_value()) return Result<void, RecipeOpError>::Err(RecipeOpError::IsPreset);

  Stmt stmt(conn, "UPDATE recipes SET name = ? WHERE id = ?;");
  sqlite3_bind_text(stmt.get(), 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.get(), 2, version_id);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("rename version failed: ") + sqlite3_errmsg(conn));
  }
  return Result<void, RecipeOpError>::Ok();
}

Result<void, RecipeOpError> delete_version(db::Database& db, RecipeId version_id) {
  sqlite3* conn = db.handle();
  auto row = get_recipe_row(conn, version_id);
  if (!row || row->deleted) return Result<void, RecipeOpError>::Err(RecipeOpError::NotFound);
  if (!row->parent_id.has_value()) return Result<void, RecipeOpError>::Err(RecipeOpError::IsPreset);

  Stmt stmt(conn, "UPDATE recipes SET deleted_at = ? WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, now_unix());
  sqlite3_bind_int64(stmt.get(), 2, version_id);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("soft-delete version failed: ") + sqlite3_errmsg(conn));
  }
  return Result<void, RecipeOpError>::Ok();
}

Result<void, SetImageRecipeError> set_image_recipe(db::Database& db, ImageId image_id,
                                                    std::optional<RecipeId> recipe_id) {
  sqlite3* conn = db.handle();
  if (!project::get_image(db, image_id)) {
    return Result<void, SetImageRecipeError>::Err(SetImageRecipeError::ImageNotFound);
  }
  if (recipe_id) {
    auto row = get_recipe_row(conn, *recipe_id);
    if (!row || row->deleted) {
      return Result<void, SetImageRecipeError>::Err(SetImageRecipeError::RecipeNotFound);
    }
  }

  Stmt stmt(conn, "UPDATE images SET recipe_id = ? WHERE id = ?;");
  if (recipe_id) {
    sqlite3_bind_int64(stmt.get(), 1, *recipe_id);
  } else {
    sqlite3_bind_null(stmt.get(), 1);
  }
  sqlite3_bind_int64(stmt.get(), 2, image_id);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    throw std::runtime_error(std::string("set image recipe failed: ") + sqlite3_errmsg(conn));
  }
  return Result<void, SetImageRecipeError>::Ok();
}

std::optional<RecipeId> get_image_recipe(db::Database& db, ImageId image_id) {
  Stmt stmt(db.handle(), "SELECT recipe_id FROM images WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, image_id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;  // 图片不存在
  if (sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL) return std::nullopt;  // 没有应用 recipe
  return sqlite3_column_int64(stmt.get(), 0);
}

namespace {

std::optional<std::string> recipe_name(sqlite3* conn, RecipeId id) {
  Stmt stmt(conn, "SELECT name FROM recipes WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
  const unsigned char* name = sqlite3_column_text(stmt.get(), 0);
  return name ? std::optional<std::string>(reinterpret_cast<const char*>(name)) : std::nullopt;
}

}  // namespace

std::optional<RecipeDescription> describe_recipe(db::Database& db, RecipeId recipe_id) {
  sqlite3* conn = db.handle();
  auto row = get_recipe_row(conn, recipe_id);
  if (!row) return std::nullopt;

  if (!row->parent_id.has_value()) {
    // 这一行本身就是预设，预设一定有名字(局部唯一索引保证非空)，没有第
    // 二层，version_name 留空。
    return RecipeDescription{recipe_name(conn, recipe_id).value_or(""), std::nullopt};
  }

  std::string preset_name = recipe_name(conn, *row->parent_id).value_or("?");
  std::string version_name = recipe_name(conn, recipe_id).value_or("(未命名)");
  return RecipeDescription{preset_name, version_name};
}

namespace {

std::optional<color::Lut3D> load_lut(sqlite3* conn, RecipeId preset_id) {
  Stmt stmt(conn, "SELECT base_lut_size, base_lut FROM recipes WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, preset_id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;

  int size = sqlite3_column_int(stmt.get(), 0);
  const void* blob = sqlite3_column_blob(stmt.get(), 1);
  int blob_bytes = sqlite3_column_bytes(stmt.get(), 1);
  if (!blob || size <= 0) return std::nullopt;

  color::Lut3D lut;
  lut.size = size;
  lut.data.resize(static_cast<std::size_t>(blob_bytes) / sizeof(float));
  std::memcpy(lut.data.data(), blob, static_cast<std::size_t>(blob_bytes));
  return lut;
}

}  // namespace

std::optional<ResolvedRecipe> resolve_recipe(db::Database& db, RecipeId recipe_id) {
  sqlite3* conn = db.handle();
  Stmt stmt(conn,
            "SELECT parent_id, highlights, shadows, wb_shift_r, wb_shift_b FROM recipes "
            "WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, recipe_id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;

  bool is_preset = sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL;
  RecipeId preset_id = is_preset ? recipe_id : sqlite3_column_int64(stmt.get(), 0);
  VersionParams params;  // 指向预设本身时保持全零(中性状态)
  if (!is_preset) {
    params.highlights = sqlite3_column_double(stmt.get(), 1);
    params.shadows = sqlite3_column_double(stmt.get(), 2);
    params.wb_shift_r = sqlite3_column_double(stmt.get(), 3);
    params.wb_shift_b = sqlite3_column_double(stmt.get(), 4);
  }

  // load_lut 返回空是合法状态(比如 Origin 这个预设本身就没有
  // base_lut),不是错误——不在这里拒绝,直接把"没有 LUT"这个事实带出去,
  // 交给 render 决定要不要跳过 apply_lut。
  return ResolvedRecipe{load_lut(conn, preset_id), params};
}

Result<decode::DecodedImage, RenderRecipeError> render(db::Database& db,
                                                        const decode::DecodedImage& src,
                                                        RecipeId recipe_id,
                                                        unsigned thread_count) {
  auto resolved = resolve_recipe(db, recipe_id);
  if (!resolved) {
    return Result<decode::DecodedImage, RenderRecipeError>::Err(RenderRecipeError::RecipeNotFound);
  }

  decode::DecodedImage out = src;  // 拷贝一份工作缓冲区,不修改调用方传入的原图
  // 没有 LUT(比如 Origin)就跳过这一步,不做一次没有意义的恒等插值——省
  // 掉的是真实的逐像素 8 次查表计算量,不只是省一次函数调用。
  if (resolved->lut) {
    color::apply_lut(out, *resolved->lut, thread_count);
  }
  // 同样的道理对调整参数也成立:四个参数全零(比如 Origin 预设本身)时,
  // apply_adjustments 算出来的 delta 恒为 0、增益恒为 1,是个不折不扣的
  // 无意义计算——之前漏掉了这一半的优化,只跳过了 LUT。
  const auto& p = resolved->params;
  bool has_adjustments = p.highlights != 0 || p.shadows != 0 || p.wb_shift_r != 0 || p.wb_shift_b != 0;
  if (has_adjustments) {
    color::apply_adjustments(out, p.highlights, p.shadows, p.wb_shift_r, p.wb_shift_b, thread_count);
  }
  return Result<decode::DecodedImage, RenderRecipeError>::Ok(std::move(out));
}

}  // namespace pzt::core::recipe
