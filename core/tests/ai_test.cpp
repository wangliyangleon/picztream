#include <doctest.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "core/ai/ai.h"

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

// setenv/unsetenv 是全局进程状态，doctest 不保证用例之间的执行顺序或者
// 互相隔离——RAII 保存/恢复原值，析构时无条件复原，不管测试用例是正常走
// 完还是中途因为 REQUIRE 失败提前退出，都不会把状态泄漏给其它用例（比
// cli/tests/i18n_test.cpp 里手动在用例末尾重置 g_lang 更稳一些，这里选
// 择更保险的写法，因为同时要摆弄两个环境变量）。
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

TEST_CASE("request_json parses a Claude-shaped response") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200, R"({"content":[{"type":"text","text":"{\"foo\":\"bar\",\"n\":5}"}]})"});
  };

  auto result = request_json(img, "evaluate this", "respond with foo and n", Provider::Claude,
                              fake_post);
  REQUIRE(result.ok());
  CHECK(result.value()["foo"] == "bar");
  CHECK(result.value()["n"] == 5);
}

TEST_CASE("request_json parses a Gemini-shaped response") {
  EnvVarGuard key("GEMINI_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200,
        R"({"candidates":[{"content":{"parts":[{"text":"{\"foo\":\"bar\"}"}]}}]})"});
  };

  auto result =
      request_json(img, "evaluate this", "respond with foo", Provider::Gemini, fake_post);
  REQUIRE(result.ok());
  CHECK(result.value()["foo"] == "bar");
}

TEST_CASE("request_json strips a markdown code fence around the inner JSON") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{
        200, "{\"content\":[{\"type\":\"text\",\"text\":\"```json\\n{\\\"foo\\\":1}\\n```\"}]}"});
  };

  auto result = request_json(img, "p", "s", Provider::Claude, fake_post);
  REQUIRE(result.ok());
  CHECK(result.value()["foo"] == 1);
}

TEST_CASE("request_json reports HttpError for a non-2xx status code") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{500, R"({"type":"error","error":{"message":"boom"}})"});
  };

  auto result = request_json(img, "p", "s", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == RequestError::HttpError);
}

TEST_CASE("request_json reports ParseError when the inner text is not valid JSON") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, R"({"content":[{"type":"text","text":"not json at all"}]})"});
  };

  auto result = request_json(img, "p", "s", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == RequestError::ParseError);
}

TEST_CASE("request_json reports ParseError when the outer response shape is unexpected") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  auto fake_post = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, R"({"unexpected":true})"});
  };

  auto result = request_json(img, "p", "s", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == RequestError::ParseError);
}

TEST_CASE("request_json builds a request body containing the image, prompt and schema instruction") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  auto img = make_image(4, 4);

  std::string captured_body;
  std::string captured_url;
  auto fake_post = [&](const std::string& url, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_url = url;
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, R"({"content":[{"type":"text","text":"{\"ok\":true}"}]})"});
  };

  auto result = request_json(img, "some user prompt", "some schema instruction", Provider::Claude,
                              fake_post);
  REQUIRE(result.ok());
  CHECK(captured_url.find("anthropic.com") != std::string::npos);
  CHECK(captured_body.find("some user prompt") != std::string::npos);
  CHECK(captured_body.find("some schema instruction") != std::string::npos);
  // base64 编码之后的图片数据本身不好断言具体内容，只确认 body 里带了一
  // 段看起来像图片数据的字段("data")，不是空的。
  CHECK(captured_body.find("\"data\"") != std::string::npos);
}

// F-02：request_json 内部在编码上传之前会把图片降采样，纯色测试图片压
// 缩后几乎不随分辨率变化，没法从 base64 载荷大小反推有没有真的缩小，
// 直接测 detail::downscale_for_upload 的输出宽高更准确、更稳定。
TEST_CASE("downscale_for_upload shrinks large images to the upload cap, preserving aspect ratio") {
  auto wide = make_image(4000, 2000);  // 长边 4000，2:1
  auto scaled = detail::downscale_for_upload(wide);
  CHECK(scaled.width == 1024);
  CHECK(scaled.height == 512);  // 长边缩到 1024，短边按同样比例缩小
}

TEST_CASE("downscale_for_upload leaves images at or under the cap untouched") {
  auto small = make_image(800, 600);
  auto scaled = detail::downscale_for_upload(small);
  CHECK(scaled.width == 800);
  CHECK(scaled.height == 600);

  auto exact = make_image(1024, 768);
  auto scaled_exact = detail::downscale_for_upload(exact);
  CHECK(scaled_exact.width == 1024);
  CHECK(scaled_exact.height == 768);
}

TEST_CASE("request_json reports MissingApiKey without calling http_post") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  auto img = make_image(4, 4);

  bool called = false;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    called = true;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, "{}"});
  };

  auto result = request_json(img, "p", "s", Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == RequestError::MissingApiKey);
  CHECK(!called);
}

TEST_CASE("to_string maps every provider to its lowercase name") {
  CHECK(std::string(to_string(Provider::Claude)) == "claude");
  CHECK(std::string(to_string(Provider::Gemini)) == "gemini");
  CHECK(std::string(to_string(Provider::Local)) == "local");
}
