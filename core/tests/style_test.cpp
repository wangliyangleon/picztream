#include <doctest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ai/style.h"

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

// 跟 evaluation_test.cpp/ai_test.cpp 的 EnvVarGuard 是同一个写法——各自文
// 件独立一份，这类只有几行、只在测试里用的小工具没必要为了共享专门开
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

std::vector<std::string> sample_preset_names() {
  return {"Havana 1959", "Tokyo 1966", "Munich 1951"};
}

}  // namespace

TEST_CASE("request_style_suggestion_impl extracts recipe_name and reasoning") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    nlohmann::json body{{"recipe_name", "Tokyo 1966"}, {"reasoning", "warm nostalgic tones fit"}};
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(body)});
  };

  auto result = detail::request_style_suggestion_impl(img, sample_preset_names(), Provider::Claude,
                                                        fake_post);
  REQUIRE(result.ok());
  CHECK(result.value().recipe_name == "Tokyo 1966");
  CHECK(result.value().reasoning == "warm nostalgic tones fit");
}

TEST_CASE("request_style_suggestion_impl reports Hallucinated when recipe_name isn't a real candidate") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    nlohmann::json body{{"recipe_name", "Vintage Kodachrome"}, {"reasoning", "made this up"}};
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(body)});
  };

  auto result = detail::request_style_suggestion_impl(img, sample_preset_names(), Provider::Claude,
                                                        fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == StyleError::Hallucinated);
}

TEST_CASE("request_style_suggestion_impl reports ParseError when fields are missing or the wrong type") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto missing_reasoning = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                               const std::string&) -> Result<HttpResponse, RequestError> {
    nlohmann::json body{{"recipe_name", "Tokyo 1966"}};
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(body)});
  };
  auto r1 = detail::request_style_suggestion_impl(img, sample_preset_names(), Provider::Claude,
                                                    missing_reasoning);
  REQUIRE(!r1.ok());
  CHECK(r1.error() == StyleError::ParseError);

  auto wrong_type = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    nlohmann::json body{{"recipe_name", 42}, {"reasoning", "n"}};
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(body)});
  };
  auto r2 = detail::request_style_suggestion_impl(img, sample_preset_names(), Provider::Claude,
                                                    wrong_type);
  REQUIRE(!r2.ok());
  CHECK(r2.error() == StyleError::ParseError);
}

TEST_CASE("request_style_suggestion_impl maps network/http errors") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto network_fail = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                          const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Err(RequestError::NetworkError);
  };
  auto r1 = detail::request_style_suggestion_impl(img, sample_preset_names(), Provider::Claude,
                                                    network_fail);
  REQUIRE(!r1.ok());
  CHECK(r1.error() == StyleError::NetworkError);

  auto http_fail = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{500, "server error"});
  };
  auto r2 = detail::request_style_suggestion_impl(img, sample_preset_names(), Provider::Claude,
                                                    http_fail);
  REQUIRE(!r2.ok());
  CHECK(r2.error() == StyleError::HttpError);
}

TEST_CASE("request_style_suggestion_impl reports MissingApiKey without calling http_post") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  auto img = make_image(4, 4);

  bool called = false;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    called = true;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, "{}"});
  };

  auto result = detail::request_style_suggestion_impl(img, sample_preset_names(), Provider::Claude,
                                                        fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == StyleError::MissingApiKey);
  CHECK(!called);
}

TEST_CASE("request_style_suggestion (public entry point) reports MissingApiKey without a real network call") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  EnvVarGuard gemini_key("GEMINI_API_KEY", nullptr);
  auto img = make_image(4, 4);

  auto result = request_style_suggestion(img, sample_preset_names(), Provider::Claude);
  REQUIRE(!result.ok());
  CHECK(result.error() == StyleError::MissingApiKey);
}

TEST_CASE("request_style_suggestion_impl constrains recipe_name to an enum of the candidates for Provider::Local") {
  auto img = make_image(4, 4);

  std::string captured_body;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200, R"({"message":{"role":"assistant","content":"{\"recipe_name\":\"Tokyo 1966\",)"
             R"(\"reasoning\":\"n\"}"}})"});
  };

  auto result = detail::request_style_suggestion_impl(img, sample_preset_names(), Provider::Local,
                                                        fake_post);
  REQUIRE(result.ok());

  auto parsed_body = nlohmann::json::parse(captured_body);
  REQUIRE(parsed_body["format"].is_object());
  CHECK(parsed_body["format"]["properties"]["recipe_name"]["enum"] ==
        nlohmann::json(sample_preset_names()));
  CHECK(parsed_body["options"]["temperature"] == 0);
}
