#include "core/recipe/recipe.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/db/stmt.h"

namespace pzt::core::recipe {

namespace {

using db::Stmt;

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 恒等 LUT：输出 = 输入。验证"没有对颜色做任何事"这条路径的行为是正确
// 的——真正落地渲染管线(increment 4)时,拿一张图跑这个预设,像素应该原样
// 不变。
std::vector<float> make_identity_lut(int n) {
  std::vector<float> lut(static_cast<std::size_t>(n) * n * n * 3);
  for (int r = 0; r < n; ++r) {
    for (int g = 0; g < n; ++g) {
      for (int b = 0; b < n; ++b) {
        std::size_t idx = (static_cast<std::size_t>(r) * n + g) * n + b;
        lut[idx * 3 + 0] = static_cast<float>(r) / (n - 1);
        lut[idx * 3 + 1] = static_cast<float>(g) / (n - 1);
        lut[idx * 3 + 2] = static_cast<float>(b) / (n - 1);
      }
    }
  }
  return lut;
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
  sqlite3_step(stmt.get());
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
  seed_preset(db.handle(), "Standard", 17, make_identity_lut(17));
  seed_preset(db.handle(), "Warm", 17, make_warm_lut(17));
}

}  // namespace pzt::core::recipe
