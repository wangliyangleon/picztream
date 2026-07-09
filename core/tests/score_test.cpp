#include <doctest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ai/score.h"

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

// 跟 ai_test.cpp 的 EnvVarGuard 是同一个写法——各自文件独立一份，不是漏
// 了抽公共头，这类只有几行、只在测试里用的小工具没必要为了共享专门开一
// 个头文件。
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

}  // namespace

TEST_CASE("request_score_impl omits the guidance section when extra_guidance is empty") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  std::string captured_body;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, R"({"content":[{"type":"text","text":"{\"score\":50,\"comment\":\"ok\"}"}]})"});
  };

  auto result = detail::request_score_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  CHECK(captured_body.find("aesthetic quality") != std::string::npos);
  CHECK(captured_body.find("Additional guidance") == std::string::npos);
}

TEST_CASE("request_score_impl appends the extra guidance when non-empty") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  std::string captured_body;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, R"({"content":[{"type":"text","text":"{\"score\":50,\"comment\":\"ok\"}"}]})"});
  };

  auto result =
      detail::request_score_impl(img, "focus on the crop", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  CHECK(captured_body.find("Additional guidance: focus on the crop") != std::string::npos);
}

TEST_CASE("request_score_impl extracts score and comment on success") {
  EnvVarGuard key("GEMINI_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200,
        R"({"candidates":[{"content":{"parts":[{"text":"{\"score\":87,\"comment\":\"nice light\"}"}]}}]})"});
  };

  auto result = detail::request_score_impl(img, "", Provider::Gemini, fake_post);
  REQUIRE(result.ok());
  CHECK(result.value().score == 87);
  CHECK(result.value().comment == "nice light");
}

TEST_CASE("request_score_impl reports OutOfRange when score is outside 1-100") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  // 内层 text 是"JSON 字符串里的 JSON"，手写转义容易出错，直接用
  // nlohmann::json 现场序列化两层更保险。
  auto build_response = [](int score) {
    nlohmann::json inner{{"score", score}, {"comment", "x"}};
    nlohmann::json outer{{"content", nlohmann::json::array({{{"type", "text"},
                                                              {"text", inner.dump()}}})}};
    return outer.dump();
  };

  for (int bad_score : {0, 101, -5}) {
    std::string body = build_response(bad_score);
    auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                          const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, body});
    };
    auto result = detail::request_score_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == ScoreError::OutOfRange);
  }
}

TEST_CASE("request_score_impl reports ParseError when score or comment is missing or the wrong type") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto build_response = [](const nlohmann::json& inner) {
    nlohmann::json outer{{"content", nlohmann::json::array({{{"type", "text"},
                                                              {"text", inner.dump()}}})}};
    return outer.dump();
  };

  std::vector<nlohmann::json> bad_shapes = {
      {{"comment", "x"}},                 // score 缺失
      {{"score", 50}},                    // comment 缺失
      {{"score", "50"}, {"comment", "x"}},  // score 类型不对(字符串而不是整数)
  };

  for (const auto& shape : bad_shapes) {
    std::string body = build_response(shape);
    auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                          const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, body});
    };
    auto result = detail::request_score_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == ScoreError::ParseError);
  }
}

TEST_CASE("request_score_impl maps RequestError to the matching ScoreError") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{500, "server error"});
  };

  auto result = detail::request_score_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == ScoreError::HttpError);
}

TEST_CASE("request_score_impl reports MissingApiKey without calling http_post") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  auto img = make_image(4, 4);

  bool called = false;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    called = true;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, "{}"});
  };

  auto result = detail::request_score_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == ScoreError::MissingApiKey);
  CHECK(!called);
}

TEST_CASE("request_score (public entry point) reports MissingApiKey without a real network call") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  EnvVarGuard gemini_key("GEMINI_API_KEY", nullptr);
  auto img = make_image(4, 4);

  auto result = request_score(img, "", Provider::Claude);
  REQUIRE(!result.ok());
  CHECK(result.error() == ScoreError::MissingApiKey);
}
