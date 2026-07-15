#include <doctest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ai/evaluation.h"

using namespace pzt::core::ai;
using pzt::core::Result;
using pzt::core::decode::DecodedImage;

namespace {

DecodedImage make_image(int width, int height) {
  DecodedImage img;
  img.width = width;
  img.height = height;
  img.rgba.resize(static_cast<std::size_t>(width) * height * 4, 128);
  return img;
}

// 跟 ai_test.cpp 的 EnvVarGuard 是同一个写法——各自文件独立一份，这类只
// 有几行、只在测试里用的小工具没必要为了共享专门开一个头文件。
struct EnvVarGuard {
  std::string name;
  std::optional<std::string> previous;

  EnvVarGuard(std::string n, const char* value) : name(std::move(n)) {
    const char* existing = std::getenv(name.c_str());
    if (existing) previous = existing;
    if (value) {
      setenv(name.c_str(), value, 1);
    } else {
      unsetenv(name.c_str());
    }
  }

  ~EnvVarGuard() {
    if (previous) {
      setenv(name.c_str(), previous->c_str(), 1);
    } else {
      unsetenv(name.c_str());
    }
  }
};

std::string wrap_claude_response(const nlohmann::json& inner) {
  nlohmann::json outer{
      {"content", nlohmann::json::array({{{"type", "text"}, {"text", inner.dump()}}})}};
  return outer.dump();
}

nlohmann::json full_response_json(bool with_fixes) {
  nlohmann::json exposure{{"score", 7}, {"note", "slightly underexposed"}};
  if (with_fixes) exposure["fix_percent"] = 15;

  nlohmann::json composition{{"score", 4}, {"note", "horizon is tilted"}};
  if (with_fixes) {
    composition["fix"] = {{"rotate_degrees", 2.5},
                           {"crop_left_percent", 0},
                           {"crop_right_percent", 0},
                           {"crop_top_percent", 0},
                           {"crop_bottom_percent", 5}};
  }

  nlohmann::json focus{{"score", 9}, {"note", "sharp"}};

  return {{"exposure", exposure},
          {"composition", composition},
          {"focus", focus},
          {"comment", "overall solid, mainly the tilted horizon"}};
}

}  // namespace

TEST_CASE("request_evaluation_impl extracts all three dimensions and the overall comment") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, wrap_claude_response(full_response_json(/*with_fixes=*/true))});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  const auto& r = result.value();
  CHECK(r.exposure.score == 7);
  CHECK(r.exposure.note == "slightly underexposed");
  CHECK(r.composition.score == 4);
  CHECK(r.composition.note == "horizon is tilted");
  CHECK(r.focus.score == 9);
  CHECK(r.focus.note == "sharp");
  CHECK(r.comment == "overall solid, mainly the tilted horizon");
}

TEST_CASE("request_evaluation_impl parses fix suggestions when present") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, wrap_claude_response(full_response_json(/*with_fixes=*/true))});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  const auto& r = result.value();
  REQUIRE(r.exposure_fix.has_value());
  CHECK(r.exposure_fix->adjust_percent == doctest::Approx(15.0));
  REQUIRE(r.composition_fix.has_value());
  CHECK(r.composition_fix->rotate_degrees == doctest::Approx(2.5));
  CHECK(r.composition_fix->crop_bottom_percent == doctest::Approx(5.0));
}

TEST_CASE("request_evaluation_impl omits fix suggestions when the model doesn't provide them") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, wrap_claude_response(full_response_json(/*with_fixes=*/false))});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  CHECK(!result.value().exposure_fix.has_value());
  CHECK(!result.value().composition_fix.has_value());
}

TEST_CASE("request_evaluation_impl treats a malformed fix as absent, not a hard failure") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  nlohmann::json inner = full_response_json(/*with_fixes=*/false);
  inner["exposure"]["fix_percent"] = "not a number";  // 类型不对

  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(inner)});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());  // 整体评估不因为这一个次要字段类型不对就整体失败
  CHECK(!result.value().exposure_fix.has_value());
}

TEST_CASE("request_evaluation_impl requires all five composition fix sub-fields together") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  nlohmann::json inner = full_response_json(/*with_fixes=*/true);
  inner["composition"]["fix"].erase("crop_bottom_percent");  // 缺一个子字段

  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(inner)});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  CHECK(!result.value().composition_fix.has_value());
}

TEST_CASE("request_evaluation_impl reports OutOfRange when a dimension score is outside 0-10") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  nlohmann::json inner = full_response_json(/*with_fixes=*/false);
  inner["focus"]["score"] = 11;

  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(inner)});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == EvaluationError::OutOfRange);
}

TEST_CASE("request_evaluation_impl reports ParseError when a dimension or comment is missing") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  nlohmann::json inner = full_response_json(/*with_fixes=*/false);
  inner.erase("composition");

  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(inner)});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == EvaluationError::ParseError);
}

TEST_CASE("request_evaluation_impl maps RequestError to the matching EvaluationError") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{500, "server error"});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == EvaluationError::HttpError);
}

TEST_CASE("request_evaluation_impl reports MissingApiKey without calling http_post") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  auto img = make_image(4, 4);

  bool called = false;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    called = true;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, "{}"});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == EvaluationError::MissingApiKey);
  CHECK(!called);
}

TEST_CASE("request_evaluation_impl's prompt omits guidance when empty, includes it when given") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  std::string captured_empty;
  auto fake_post_empty =
      [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
          const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_empty = body;
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, wrap_claude_response(full_response_json(false))});
  };
  auto r1 = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post_empty);
  REQUIRE(r1.ok());
  CHECK(captured_empty.find("Additional guidance") == std::string::npos);

  std::string captured_with_guidance;
  auto fake_post_guided =
      [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
          const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_with_guidance = body;
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, wrap_claude_response(full_response_json(false))});
  };
  auto r2 = detail::request_evaluation_impl(img, "pay attention to the crop",
                                              Provider::Claude, fake_post_guided);
  REQUIRE(r2.ok());
  CHECK(captured_with_guidance.find("Additional guidance: pay attention to the crop") !=
        std::string::npos);
}

TEST_CASE("request_evaluation (public entry point) reports MissingApiKey without a real network call") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  EnvVarGuard gemini_key("GEMINI_API_KEY", nullptr);
  auto img = make_image(4, 4);

  auto result = request_evaluation(img, "", Provider::Claude);
  REQUIRE(!result.ok());
  CHECK(result.error() == EvaluationError::MissingApiKey);
}

TEST_CASE("request_evaluation_impl passes a real JSON Schema in format for Provider::Local") {
  auto img = make_image(4, 4);

  std::string captured_body;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200,
        R"({"message":{"role":"assistant","content":"{\"exposure\":{\"score\":7,\"note\":\"n\"},)"
        R"(\"composition\":{\"score\":7,\"note\":\"n\"},\"focus\":{\"score\":7,\"note\":\"n\"},)"
        R"(\"comment\":\"c\"}"}})"});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Local, fake_post);
  REQUIRE(result.ok());

  auto parsed_body = nlohmann::json::parse(captured_body);
  REQUIRE(parsed_body["format"].is_object());  // 不是宽松的 "json" 字符串，是真的 schema 对象
  CHECK(parsed_body["format"]["type"] == "object");
  CHECK(parsed_body["format"]["properties"].contains("exposure"));
  CHECK(parsed_body["format"]["properties"].contains("composition"));
  CHECK(parsed_body["format"]["properties"].contains("focus"));
  CHECK(parsed_body["format"]["properties"].contains("comment"));
  CHECK(parsed_body["options"]["temperature"] == 0);
}

TEST_CASE("overall_score averages the three dimensions and rounds") {
  EvaluationInfo info{
      DimensionAssessment{7, ""}, std::nullopt, DimensionAssessment{7, ""}, std::nullopt,
      DimensionAssessment{8, ""}, "", "", "",
  };
  CHECK(overall_score(info) == 7);  // (7+7+8)/3 = 7.33 -> 7

  info.focus.score = 9;
  CHECK(overall_score(info) == 8);  // (7+7+9)/3 = 7.67 -> 8
}

TEST_CASE("passes_gate requires every dimension to meet the threshold") {
  EvaluationInfo info{
      DimensionAssessment{6, ""}, std::nullopt, DimensionAssessment{6, ""}, std::nullopt,
      DimensionAssessment{6, ""}, "", "", "",
  };
  CHECK(passes_gate(info));

  info.composition.score = 5;
  CHECK(!passes_gate(info));
}
