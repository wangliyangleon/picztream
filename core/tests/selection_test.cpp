#include <doctest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ai/selection.h"

using namespace pzt::core::ai;
using pzt::core::Result;

namespace {

// 跟 compare_test.cpp/evaluation_test.cpp 的 EnvVarGuard 是同一个写法，各自
// 文件独立一份是既有惯例。
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

std::vector<SelectionCandidate> three_candidates() {
  return {
      SelectionCandidate{"锐利、构图均衡", "一只猫趴在窗台上晒太阳"},
      SelectionCandidate{"轻微欠曝", "海边的日落，两个人手牵手走着"},
      SelectionCandidate{"对焦准确、色彩浓郁", "夜市摊位前热闹的人群"},
  };
}

// 只回一个固定 picks 的假 http_post，同时把请求体录下来供提示词形状断言。
auto capturing_post(std::string& captured_body, nlohmann::json inner) {
  return [&captured_body, inner](const std::string&,
                                  const std::vector<std::pair<std::string, std::string>>&,
                                  const std::string& body) -> Result<HttpResponse, RequestError> {
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, wrap_claude_response(inner)});
  };
}

// Claude 请求体里那段发给模型的完整指令文本（图片列表为空，只剩一条 text）。
std::string claude_instruction_text(const std::string& body) {
  auto parsed = nlohmann::json::parse(body);
  const auto& content = parsed["messages"][0]["content"];
  REQUIRE(content.is_array());
  for (const auto& part : content) {
    if (part.value("type", "") == "text") return part["text"].get<std::string>();
  }
  return "";
}

}  // namespace

TEST_CASE("request_selection_impl returns the model's picks in the order the model gave them") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  std::string body;
  auto post = capturing_post(body, nlohmann::json{{"picks", nlohmann::json::array({3, 1})}});

  auto result = detail::request_selection_impl(three_candidates(), /*count=*/2, Provider::Claude,
                                                post);
  REQUIRE(result.ok());
  // 顺序即交付顺序（PRD 决策十四），这一层原样交还，不排序也不清洗。
  CHECK(result.value().picks == std::vector<int>{3, 1});
}

TEST_CASE("request_selection_impl hands the model both the quality assessment and the content description") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  std::string body;
  auto post = capturing_post(body, nlohmann::json{{"picks", nlohmann::json::array({1})}});

  auto result = detail::request_selection_impl(three_candidates(), /*count=*/1, Provider::Claude,
                                                post);
  REQUIRE(result.ok());

  // PRD 决策六：两个字段一起进上下文。只传 content 会丢掉质量维度，而几张
  // 都符合题材时，决定选谁的恰恰是 assessment。
  std::string prompt = claude_instruction_text(body);
  CHECK(prompt.find("锐利、构图均衡") != std::string::npos);
  CHECK(prompt.find("一只猫趴在窗台上晒太阳") != std::string::npos);
  CHECK(prompt.find("夜市摊位前热闹的人群") != std::string::npos);
  // 每张照片带着它的序号出现（模型只吐序号，PRD 决策十三）。
  CHECK(prompt.find("1.") != std::string::npos);
  CHECK(prompt.find("3.") != std::string::npos);
}

TEST_CASE("request_selection_impl puts the selection brief in the prompt only when it is non-empty") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");

  std::string with_brief_body;
  auto with_brief = capturing_post(with_brief_body, nlohmann::json{{"picks", nlohmann::json::array({1})}});
  auto r1 = detail::request_selection_impl(three_candidates(), 1, Provider::Claude, with_brief,
                                            /*selection_brief=*/"多要几张有人的，发朋友圈");
  REQUIRE(r1.ok());
  CHECK(claude_instruction_text(with_brief_body).find("多要几张有人的，发朋友圈") !=
        std::string::npos);

  // 票 08 之前没有人传选片简述，为空时整段省略而不是留一句空的"用户要求："。
  std::string empty_brief_body;
  auto empty_brief = capturing_post(empty_brief_body, nlohmann::json{{"picks", nlohmann::json::array({1})}});
  auto r2 = detail::request_selection_impl(three_candidates(), 1, Provider::Claude, empty_brief,
                                            /*selection_brief=*/"");
  REQUIRE(r2.ok());
  std::string prompt = claude_instruction_text(empty_brief_body);
  CHECK(prompt.find("The user asked for") == std::string::npos);
}

TEST_CASE("request_selection_impl sends no images and constrains picks to integers for Provider::Local") {
  std::string captured_body;
  auto fake_post = [&](const std::string& url,
                        const std::vector<std::pair<std::string, std::string>>&,
                        const std::string& body) -> Result<HttpResponse, RequestError> {
    CHECK(url.find("/api/chat") != std::string::npos);
    captured_body = body;
    return Result<HttpResponse, RequestError>::Ok(
        HttpResponse{200, R"({"message":{"role":"assistant","content":"{\"picks\":[2,1]}"}})"});
  };

  auto result = detail::request_selection_impl(three_candidates(), 2, Provider::Local, fake_post);
  REQUIRE(result.ok());
  CHECK(result.value().picks == std::vector<int>{2, 1});

  auto parsed_body = nlohmann::json::parse(captured_body);
  // 纯文本路径：一张图都不发，连 images 这个键都不该出现。
  CHECK_FALSE(parsed_body["messages"][0].contains("images"));
  // 约束解码：picks 是整数数组，幻觉面缩成"整数在不在范围内"。
  REQUIRE(parsed_body["format"].is_object());
  CHECK(parsed_body["format"]["properties"]["picks"]["type"] == "array");
  CHECK(parsed_body["format"]["properties"]["picks"]["items"]["type"] == "integer");
  CHECK(parsed_body["options"]["temperature"] == 0);
}

TEST_CASE("request_selection_impl skips non-integer entries instead of discarding the whole array") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");
  std::string body;
  // 决策十三的立场往上一层也成立：局部不合法不该把真信号一起扔掉。越界与
  // 重复由 curate 的清洗函数管，这一层只负责"能读出哪些整数"。
  auto post = capturing_post(
      body, nlohmann::json{{"picks", nlohmann::json::array({2, "three", 1, nullptr})}});

  auto result = detail::request_selection_impl(three_candidates(), 3, Provider::Claude, post);
  REQUIRE(result.ok());
  CHECK(result.value().picks == std::vector<int>{2, 1});
}

TEST_CASE("request_selection_impl reports ParseError when picks is missing or not an array") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");

  std::string b1;
  auto missing = capturing_post(b1, nlohmann::json{{"reasoning", "picked the best ones"}});
  auto r1 = detail::request_selection_impl(three_candidates(), 2, Provider::Claude, missing);
  REQUIRE(!r1.ok());
  CHECK(r1.error() == SelectionError::ParseError);

  std::string b2;
  auto wrong_type = capturing_post(b2, nlohmann::json{{"picks", "1, 2"}});
  auto r2 = detail::request_selection_impl(three_candidates(), 2, Provider::Claude, wrong_type);
  REQUIRE(!r2.ok());
  CHECK(r2.error() == SelectionError::ParseError);
}

TEST_CASE("request_selection_impl maps network and http errors") {
  EnvVarGuard key("ANTHROPIC_API_KEY", "fake-key-for-test");

  auto network_fail = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                          const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Err(RequestError::NetworkError);
  };
  auto r1 = detail::request_selection_impl(three_candidates(), 2, Provider::Claude, network_fail);
  REQUIRE(!r1.ok());
  CHECK(r1.error() == SelectionError::NetworkError);

  auto http_fail = [](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                       const std::string&) -> Result<HttpResponse, RequestError> {
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{500, "server error"});
  };
  auto r2 = detail::request_selection_impl(three_candidates(), 2, Provider::Claude, http_fail);
  REQUIRE(!r2.ok());
  CHECK(r2.error() == SelectionError::HttpError);
}

TEST_CASE("request_selection_impl reports MissingApiKey without calling http_post") {
  EnvVarGuard key("ANTHROPIC_API_KEY", nullptr);

  bool called = false;
  auto fake_post = [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> Result<HttpResponse, RequestError> {
    called = true;
    return Result<HttpResponse, RequestError>::Ok(HttpResponse{200, "{}"});
  };

  auto result = detail::request_selection_impl(three_candidates(), 2, Provider::Claude, fake_post);
  REQUIRE(!result.ok());
  CHECK(result.error() == SelectionError::MissingApiKey);
  CHECK(!called);
}

TEST_CASE("request_selection (public entry point) reports MissingApiKey without a real network call") {
  EnvVarGuard claude_key("ANTHROPIC_API_KEY", nullptr);
  EnvVarGuard gemini_key("GEMINI_API_KEY", nullptr);

  auto result = request_selection(three_candidates(), 2, Provider::Claude);
  REQUIRE(!result.ok());
  CHECK(result.error() == SelectionError::MissingApiKey);
}
