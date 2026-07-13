#include "core/settings/settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace pzt::core::settings {

namespace fs = std::filesystem;

std::string default_config_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  fs::path config_home =
      (xdg && *xdg) ? fs::path(xdg) : fs::path(std::getenv("HOME")) / ".config";
  return (config_home / "pzt" / "config.json").string();
}

namespace {

std::optional<ai::Provider> parse_provider(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower == "claude") return ai::Provider::Claude;
  if (lower == "gemini") return ai::Provider::Gemini;
  return std::nullopt;
}

// 每个字段独立容错：key 缺失、或者类型对不上，都保留 out 已有的值不
// 动(调用方在传进来之前已经是默认值)，不让一个字段的问题连累其它已
// 经解析成功的字段。
template <typename T>
void assign_if_present(const nlohmann::json& j, const char* key, T& out) {
  if (!j.contains(key) || j[key].is_null()) return;
  try {
    out = j.at(key).get<T>();
  } catch (const nlohmann::json::exception&) {
    // 类型不对，保留原值
  }
}

}  // namespace

Settings load(const std::string& path) {
  Settings settings;

  std::ifstream file(path);
  if (!file) return settings;  // 文件不存在，全部用默认值

  nlohmann::json j;
  try {
    file >> j;
  } catch (const nlohmann::json::parse_error&) {
    return settings;  // 整个文件不是合法 JSON，全部用默认值
  }
  if (!j.is_object()) return settings;

  if (j.contains("ai_provider") && j["ai_provider"].is_string()) {
    if (auto provider = parse_provider(j["ai_provider"].get<std::string>())) {
      settings.ai_provider = *provider;
    }
  }
  assign_if_present(j, "dedup_time_window_seconds", settings.dedup_time_window_seconds);
  assign_if_present(j, "dedup_hash_threshold", settings.dedup_hash_threshold);
  assign_if_present(j, "curate_time_window_seconds", settings.curate_time_window_seconds);
  assign_if_present(j, "curate_hash_threshold", settings.curate_hash_threshold);
  assign_if_present(j, "eval_reject", settings.eval_reject);
  assign_if_present(j, "dedup_reject", settings.dedup_reject);
  assign_if_present(j, "export_reject", settings.export_reject);
  assign_if_present(j, "export_dup", settings.export_dup);
  assign_if_present(j, "auto_ai_reject", settings.auto_ai_reject);
  if (j.contains("lang") && j["lang"].is_string()) {
    settings.lang = j["lang"].get<std::string>();
  }
  assign_if_present(j, "ui_width_ratio", settings.ui_width_ratio);
  assign_if_present(j, "prefetch_window", settings.prefetch_window);

  return settings;
}

}  // namespace pzt::core::settings
