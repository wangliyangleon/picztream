#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/settings/settings.h"

namespace fs = std::filesystem;
using pzt::core::ai::Provider;
using pzt::core::settings::load;
using pzt::core::settings::Settings;

namespace {

std::string fresh_config_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / ("settings_" + tag + ".json")).string();
  fs::remove(path);
  return path;
}

void write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path);
  f << content;
}

}  // namespace

TEST_CASE("load returns all defaults when the config file doesn't exist") {
  auto path = fresh_config_path("missing");
  Settings s = load(path);
  CHECK(s.ai_provider == Provider::Local);
  CHECK(s.ollama_base_url == "http://localhost:11434");
  CHECK(s.ollama_model == "gemma4:e2b");
  CHECK(s.dedup_time_window_seconds == 10);
  CHECK(s.dedup_hash_threshold == 5);
  CHECK(s.curate_time_window_seconds == 20);
  CHECK(s.curate_hash_threshold == 10);
  CHECK(s.eval_reject == false);
  CHECK(s.dedup_reject == false);
  CHECK(s.export_reject == false);
  CHECK(s.export_dup == false);
  CHECK(s.auto_ai_reject == false);
  CHECK(!s.lang.has_value());
  CHECK(s.ui_width_ratio == doctest::Approx(0.7));
  CHECK(s.prefetch_window == 3);
}

TEST_CASE("load returns all defaults when the file is not valid JSON at all") {
  auto path = fresh_config_path("malformed");
  write_file(path, "{ this is not json");
  Settings s = load(path);
  CHECK(s.ai_provider == Provider::Local);
  CHECK(s.dedup_time_window_seconds == 10);
}

TEST_CASE("load returns all defaults when the top-level JSON value isn't an object") {
  auto path = fresh_config_path("not_object");
  write_file(path, "[1, 2, 3]");
  Settings s = load(path);
  CHECK(s.dedup_time_window_seconds == 10);
}

TEST_CASE("load reads every field correctly when the file is fully populated") {
  auto path = fresh_config_path("full");
  write_file(path, R"json({
    "ai_provider": "claude",
    "ollama_base_url": "http://example:1234",
    "ollama_model": "custom-model",
    "dedup_time_window_seconds": 20,
    "dedup_hash_threshold": 8,
    "curate_time_window_seconds": 30,
    "curate_hash_threshold": 15,
    "eval_reject": true,
    "dedup_reject": true,
    "export_reject": true,
    "export_dup": true,
    "auto_ai_reject": true,
    "lang": "en",
    "ui_width_ratio": 0.85,
    "prefetch_window": 5
  })json");

  Settings s = load(path);
  CHECK(s.ai_provider == Provider::Claude);
  CHECK(s.ollama_base_url == "http://example:1234");
  CHECK(s.ollama_model == "custom-model");
  CHECK(s.dedup_time_window_seconds == 20);
  CHECK(s.dedup_hash_threshold == 8);
  CHECK(s.curate_time_window_seconds == 30);
  CHECK(s.curate_hash_threshold == 15);
  CHECK(s.eval_reject == true);
  CHECK(s.dedup_reject == true);
  CHECK(s.export_reject == true);
  CHECK(s.export_dup == true);
  CHECK(s.auto_ai_reject == true);
  REQUIRE(s.lang.has_value());
  CHECK(*s.lang == "en");
  CHECK(s.ui_width_ratio == doctest::Approx(0.85));
  CHECK(s.prefetch_window == 5);
}

// F-12：局部容错——一个字段类型不对/取值不认识，不该拖累其它合法字段
// 也一起回退到默认值。
TEST_CASE("load falls back per-field on bad types or unrecognized values, others still apply") {
  auto path = fresh_config_path("partial_bad");
  write_file(path, R"json({
    "ai_provider": "not-a-real-provider",
    "dedup_time_window_seconds": "twenty",
    "dedup_hash_threshold": 8,
    "curate_hash_threshold": "ten",
    "eval_reject": "yes"
  })json");

  Settings s = load(path);
  CHECK(s.ai_provider == Provider::Local);           // 不认识的供应商名字，回退默认
  CHECK(s.dedup_time_window_seconds == 10);          // 字符串类型不对，回退默认
  CHECK(s.dedup_hash_threshold == 8);                // 这个字段本身合法，正常生效
  CHECK(s.curate_hash_threshold == 10);              // 字符串类型不对，回退默认
  CHECK(s.eval_reject == false);                     // 字符串类型不对，回退默认
}

TEST_CASE("load leaves missing fields at their default, only applies fields present") {
  auto path = fresh_config_path("partial_missing");
  write_file(path, R"json({"ui_width_ratio": 0.5})json");

  Settings s = load(path);
  CHECK(s.ui_width_ratio == doctest::Approx(0.5));
  CHECK(s.ai_provider == Provider::Local);
  CHECK(s.dedup_time_window_seconds == 10);
  CHECK(!s.lang.has_value());
}

TEST_CASE("load recognizes local as a valid ai_provider value") {
  auto path = fresh_config_path("local_provider");
  write_file(path, R"json({"ai_provider": "local"})json");

  Settings s = load(path);

  CHECK(s.ai_provider == Provider::Local);
}
