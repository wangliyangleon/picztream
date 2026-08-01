#include <doctest.h>

#include <algorithm>
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

// content 带默认值：绝大多数用例只关心 assessment/unusable，但提取守卫对
// content 同样严格(缺字段整条算 ParseError)，所以每个"应当成功"的假响应都
// 得带上它。给默认值省掉在每个调用点重复一句无关的文案。
nlohmann::json response_json(const std::string& assessment, bool unusable,
                            const std::string& content = "two people on a beach at sunset") {
  return {{"assessment", assessment}, {"unusable", unusable}, {"content", content}};
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
          HttpResponse{200, wrap_claude_response({{"unusable", false}, {"content", "a cat"}})});
    };
    auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == EvaluationError::ParseError);
  }

  SUBCASE("wrong type") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(HttpResponse{
          200,
          wrap_claude_response({{"assessment", 42}, {"unusable", false}, {"content", "a cat"}})});
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
          HttpResponse{200, wrap_claude_response({{"assessment", "ok"}, {"content", "a cat"}})});
    };
    auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == EvaluationError::ParseError);
  }

  SUBCASE("not a boolean") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(HttpResponse{
          200, wrap_claude_response(
                   {{"assessment", "ok"}, {"unusable", "yes"}, {"content", "a cat"}})});
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

TEST_CASE("request_evaluation_impl's prompt always sets the assessment language from the param") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto capture = [&](Language language, const std::string& guidance) -> std::string {
    std::string body;
    auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                          const std::string& b) -> Result<HttpResponse, RequestError> {
      body = b;
      return Result<HttpResponse, RequestError>::Ok(
          HttpResponse{200, wrap_claude_response(response_json("ok", false))});
    };
    auto r = detail::request_evaluation_impl(img, guidance, Provider::Claude, fake_post, language);
    REQUIRE(r.ok());
    return body;
  };

  CHECK(capture(Language::Chinese, "").find("assessment in Chinese") != std::string::npos);
  CHECK(capture(Language::English, "").find("assessment in English") != std::string::npos);
  // 语言不跟随 guidance——即便 guidance 是英文，Chinese 参数仍要求中文。
  CHECK(capture(Language::Chinese, "focus on the crop").find("assessment in Chinese") !=
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
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200,
                      R"({"message":{"role":"assistant","content":"{\"assessment\":\"ok\",)"
                      R"(\"unusable\":false,\"content\":\"a dog in a park\"}"}})"});
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

// content 的形状在三处被定义(提示词/schema instruction/约束解码 JSON Schema)，
// 漏掉第三处会让本地模型不稳定地吐这个字段——这一条就是钉住第三处的。
TEST_CASE("the local constrained-decoding schema requires content, not just assessment/unusable") {
  auto img = make_image(4, 4);

  std::string captured_body;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200,
                      R"({"message":{"role":"assistant","content":"{\"assessment\":\"ok\",)"
                      R"(\"unusable\":false,\"content\":\"a dog in a park\"}"}})"});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Local, fake_post);
  REQUIRE(result.ok());

  auto parsed_body = nlohmann::json::parse(captured_body);
  CHECK(parsed_body["format"]["properties"].contains("content"));
  CHECK(parsed_body["format"]["properties"]["content"]["type"] == "string");
  // 只出现在 properties 里不够——不进 required，模型照样可以省略它。
  auto required = parsed_body["format"]["required"];
  REQUIRE(required.is_array());
  CHECK(std::find(required.begin(), required.end(), "content") != required.end());
}

TEST_CASE("request_evaluation_impl extracts content alongside the assessment") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200, wrap_claude_response(response_json("sharp, well composed", false,
                                                 "two kids laughing on a beach at sunset"))});
  };

  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  const auto& r = result.value();
  CHECK(r.content == "two kids laughing on a beach at sunset");
  // 两个字段互不串味。
  CHECK(r.assessment == "sharp, well composed");
}

// 严格而非宽松退化为空：`image_evaluations` 以 image_id 为主键，缓存判据是
// "有评估记录就跳过"(PRD 决策七)，一条 content 为空的记录永远不会被刷新。
// 宁可整条失败让下次 run 重跑，也不留哑数据。
TEST_CASE("request_evaluation_impl reports ParseError when content is missing or wrong type") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  SUBCASE("missing") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(
          HttpResponse{200, wrap_claude_response({{"assessment", "ok"}, {"unusable", false}})});
    };
    auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == EvaluationError::ParseError);
  }

  SUBCASE("wrong type") {
    auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                         const std::string&) -> Result<HttpResponse, RequestError> {
      return Result<HttpResponse, RequestError>::Ok(HttpResponse{
          200, wrap_claude_response(
                   {{"assessment", "ok"}, {"unusable", false}, {"content", 42}})});
    };
    auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
    REQUIRE(!result.ok());
    CHECK(result.error() == EvaluationError::ParseError);
  }
}

// PRD 风险二(高)：提示词不明确的话，模型会顺着 assessment 的调子继续写摄影
// 评语，两个字段变成同义反复，下游文案就只能是空洞的漂亮话。
TEST_CASE("request_evaluation_impl's prompt asks for scene content, distinct from the assessment") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  std::string captured;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    captured = body;
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, wrap_claude_response(response_json("ok", false))});
  };
  auto result = detail::request_evaluation_impl(img, "", Provider::Claude, fake_post);
  REQUIRE(result.ok());

  // 四个要素都要点名，否则模型只会挑着答。
  CHECK(captured.find("what is happening") != std::string::npos);
  CHECK(captured.find("who") != std::string::npos);
  CHECK(captured.find("where") != std::string::npos);
  CHECK(captured.find("mood") != std::string::npos);
  // 并且明确要求别重复 assessment 那套摄影评语。
  CHECK(captured.find("Do not repeat") != std::string::npos);
}
