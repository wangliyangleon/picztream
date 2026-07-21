#include <doctest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ai/compare.h"

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

// 跟 evaluation_test.cpp/style_test.cpp 的 EnvVarGuard 是同一个写法——各自
// 文件独立一份，这类只有几行、只在测试里用的小工具没必要为了共享专门开
// 一个头文件。
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

}  // namespace

TEST_CASE("request_comparison_impl maps winner a/b to index 0/1 and extracts reasoning") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto a = make_image(4, 4);
  auto b = make_image(4, 4);

  SUBCASE("winner a -> 0") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      nlohmann::json body{{"winner", "a"}, {"reasoning", "stronger composition"}};
      return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(body)});
    };
    auto result = detail::request_comparison_impl(a, b, Provider::Claude, fake_post);
    REQUIRE(result.ok());
    CHECK(result.value().winner == 0);
    CHECK(result.value().reasoning == "stronger composition");
  }

  SUBCASE("winner b -> 1") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      nlohmann::json body{{"winner", "b"}, {"reasoning", "better color"}};
      return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(body)});
    };
    auto result = detail::request_comparison_impl(a, b, Provider::Claude, fake_post);
    REQUIRE(result.ok());
    CHECK(result.value().winner == 1);
  }
}

TEST_CASE("request_comparison_impl reports InvalidWinner when winner isn't a or b") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto a = make_image(4, 4);
  auto b = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    nlohmann::json body{{"winner", "tie"}, {"reasoning", "cannot decide"}};
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(body)});
  };

  auto result = detail::request_comparison_impl(a, b, Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == CompareError::InvalidWinner);
}

TEST_CASE("request_comparison_impl reports ParseError when fields are missing or the wrong type") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto a = make_image(4, 4);
  auto b = make_image(4, 4);

  auto missing_reasoning = [](const std::string&,
                               const std::vector<std::pair<std::string, std::string>>&,
                               const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, wrap_claude_response({{"winner", "a"}})});
  };
  auto r1 = detail::request_comparison_impl(a, b, Provider::Claude, missing_reasoning);
  REQUIRE(!r1.ok());
  CHECK(r1.error() == CompareError::ParseError);

  auto wrong_type = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, wrap_claude_response({{"winner", 1}, {"reasoning", "n"}})});
  };
  auto r2 = detail::request_comparison_impl(a, b, Provider::Claude, wrong_type);
  REQUIRE(!r2.ok());
  CHECK(r2.error() == CompareError::ParseError);
}

TEST_CASE("request_comparison_impl maps network/http errors") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto a = make_image(4, 4);
  auto b = make_image(4, 4);

  auto network_fail = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                          const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Err(RequestError::NetworkError);
  };
  auto r1 = detail::request_comparison_impl(a, b, Provider::Claude, network_fail);
  REQUIRE(!r1.ok());
  CHECK(r1.error() == CompareError::NetworkError);

  auto http_fail = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{500, "server error"});
  };
  auto r2 = detail::request_comparison_impl(a, b, Provider::Claude, http_fail);
  REQUIRE(!r2.ok());
  CHECK(r2.error() == CompareError::HttpError);
}

TEST_CASE("request_comparison_impl reports MissingApiKey without calling http_post") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  auto a = make_image(4, 4);
  auto b = make_image(4, 4);

  bool called = false;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    called = true;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, "{}"});
  };

  auto result = detail::request_comparison_impl(a, b, Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == CompareError::MissingApiKey);
  CHECK(!called);
}

TEST_CASE("request_comparison (public entry point) reports MissingApiKey without a real network call") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  EnvVarGuard gemini_key("GEMINI_API_KEY", nullptr);
  auto a = make_image(4, 4);
  auto b = make_image(4, 4);

  auto result = request_comparison(a, b, Provider::Claude);
  REQUIRE(!result.ok());
  CHECK(result.error() == CompareError::MissingApiKey);
}

TEST_CASE("request_comparison_impl sends both images and a winner enum schema for Provider::Local") {
  auto a = make_image(4, 4);
  auto b = make_image(4, 4);

  std::string captured_body;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200,
        R"({"message":{"role":"assistant","content":"{\"winner\":\"a\",\"reasoning\":\"n\"}"}})"});
  };

  auto result = detail::request_comparison_impl(a, b, Provider::Local, fake_post);
  REQUIRE(result.ok());

  auto parsed_body = nlohmann::json::parse(captured_body);
  // 两张图都进了请求体(Local 的 images 数组)。
  REQUIRE(parsed_body["messages"][0]["images"].is_array());
  CHECK(parsed_body["messages"][0]["images"].size() == 2);
  // winner 用 enum ["a","b"] 硬约束。
  REQUIRE(parsed_body["format"].is_object());
  CHECK(parsed_body["format"]["properties"]["winner"]["enum"] ==
        nlohmann::json(std::vector<std::string>{"a", "b"}));
  CHECK(parsed_body["options"]["temperature"] == 0);
}
