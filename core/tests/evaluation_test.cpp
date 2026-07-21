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

nlohmann::json response_json(const std::string& assessment, bool unusable) {
  return {{"assessment", assessment}, {"unusable", unusable}};
}

}  // namespace

TEST_CASE("request_evaluation_impl extracts the assessment text and the unusable flag") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200, wrap_claude_response(response_json("balanced composition, warm color, sharp", false))});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  const auto& r = result.value();
  CHECK(r.assessment == "balanced composition, warm color, sharp");
  CHECK(r.unusable == false);
}

TEST_CASE("request_evaluation_impl parses unusable=true") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200, wrap_claude_response(response_json("subject badly out of focus", true))});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  CHECK(result.value().unusable == true);
}

TEST_CASE("request_evaluation_impl reports ParseError when assessment is missing or wrong type") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  SUBCASE("missing") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(
          HttpResponse{200, wrap_claude_response({{"unusable", false}})});
    };
    auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == EvaluationError::ParseError);
  }

  SUBCASE("wrong type") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(
          HttpResponse{200, wrap_claude_response({{"assessment", 42}, {"unusable", false}})});
    };
    auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == EvaluationError::ParseError);
  }
}

TEST_CASE("request_evaluation_impl reports ParseError when unusable is missing or not a boolean") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  SUBCASE("missing") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(
          HttpResponse{200, wrap_claude_response({{"assessment", "ok"}})});
    };
    auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == EvaluationError::ParseError);
  }

  SUBCASE("not a boolean") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(
          HttpResponse{200, wrap_claude_response({{"assessment", "ok"}, {"unusable", "yes"}})});
    };
    auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == EvaluationError::ParseError);
  }
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
        HttpResponse{200, wrap_claude_response(response_json("ok", false))});
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
        HttpResponse{200, wrap_claude_response(response_json("ok", false))});
  };
  auto r2 = detail::request_evaluation_impl(img, "pay attention to the crop", Provider::Claude,
                                              fake_post_guided);
  REQUIRE(r2.ok());
  CHECK(captured_with_guidance.find("Additional guidance: pay attention to the crop") !=
        std::string::npos);
}

TEST_CASE("request_evaluation_impl's prompt sets the empty-guidance assessment language") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto capture = [&](Language language) -> std::string {
    std::string body;
    auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                          const std::string& b) -> Result<HttpResponse, RequestError> {
      body = b;
      return Result<HttpResponse, RequestError>::Ok(
          HttpResponse{200, wrap_claude_response(response_json("ok", false))});
    };
    auto r = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post, language);
    REQUIRE(r.ok());
    return body;
  };

  CHECK(capture(Language::Chinese).find("write it in Chinese") != std::string::npos);
  CHECK(capture(Language::English).find("write it in English") != std::string::npos);
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
        R"({"message":{"role":"assistant","content":"{\"assessment\":\"ok\",\"unusable\":false}"}})"});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Local, fake_post);
  REQUIRE(result.ok());

  auto parsed_body = nlohmann::json::parse(captured_body);
  REQUIRE(parsed_body["format"].is_object());  // 不是宽松的 "json" 字符串，是真的 schema 对象
  CHECK(parsed_body["format"]["type"] == "object");
  CHECK(parsed_body["format"]["properties"].contains("assessment"));
  CHECK(parsed_body["format"]["properties"].contains("unusable"));
  CHECK(parsed_body["options"]["temperature"] == 0);
}
