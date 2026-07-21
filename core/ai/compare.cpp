#include "core/ai/compare.h"

#include <vector>

namespace pzt::core::ai {

namespace {

// 发给 AI 的系统层框架指令——固定英文，跟 evaluation/style 一样不跟 cli::
// i18n 走。两张图按 request_json 多图重载的顺序排进请求体，第一张是 A、第
// 二张是 B。pairwise 假定两张都可用(可用性由 eval 的 unusable 在上游过
// 滤)，这里只比相对好坏，且必须二选一。
std::string build_compare_prompt() {
  return "You are shown two photos: the first image is A, the second image is B. Compare them "
         "on composition, color, and photographic aesthetics, and decide which is the better "
         "photo overall. You MUST pick exactly one winner - ties are not allowed. Assume both "
         "photos are usable; judge relative quality only.";
}

std::string build_compare_schema_instruction() {
  return "Return a JSON object with this exact shape: "
         "{\"winner\": <either \"a\" or \"b\", the better photo>, "
         "\"reasoning\": <one short sentence explaining the choice>}.";
}

nlohmann::json build_compare_json_schema() {
  static const char* kSchemaJson = R"json({
    "type": "object",
    "properties": {
      "winner": {"type": "string", "enum": ["a", "b"]},
      "reasoning": {"type": "string"}
    },
    "required": ["winner", "reasoning"]
  })json";
  return nlohmann::json::parse(kSchemaJson);
}

CompareError map_request_error(RequestError error) {
  switch (error) {
    case RequestError::MissingApiKey:
      return CompareError::MissingApiKey;
    case RequestError::NetworkError:
      return CompareError::NetworkError;
    case RequestError::HttpError:
      return CompareError::HttpError;
    case RequestError::ParseError:
      return CompareError::ParseError;
  }
  return CompareError::ParseError;  // 不可达，安抚 -Wreturn-type
}

}  // namespace

namespace detail {

Result<ComparisonResult, CompareError> request_comparison_impl(const decode::DecodedImage& a,
                                                                const decode::DecodedImage& b,
                                                                Provider provider,
                                                                HttpPostFn http_post,
                                                                const LocalModelConfig& local_config) {
  std::string user_prompt = build_compare_prompt();
  std::string schema_instruction = build_compare_schema_instruction();

  auto json_result =
      request_json(std::vector<decode::DecodedImage>{a, b}, user_prompt, schema_instruction, provider,
                   http_post, local_config, build_compare_json_schema());
  if (!json_result.ok()) {
    return Result<ComparisonResult, CompareError>::Err(map_request_error(json_result.error()));
  }

  const auto& j = json_result.value();
  if (!j.contains("winner") || !j["winner"].is_string() || !j.contains("reasoning") ||
      !j["reasoning"].is_string()) {
    return Result<ComparisonResult, CompareError>::Err(CompareError::ParseError);
  }

  std::string winner = j["winner"].get<std::string>();
  int winner_index;
  if (winner == "a") {
    winner_index = 0;
  } else if (winner == "b") {
    winner_index = 1;
  } else {
    // 模型没按 enum 给("A"/"tie"/别的)——跟 style 的 Hallucinated 同理，
    // bracket 需要确定的胜者，模糊结果整体算失败。
    return Result<ComparisonResult, CompareError>::Err(CompareError::InvalidWinner);
  }

  return Result<ComparisonResult, CompareError>::Ok(
      ComparisonResult{winner_index, j["reasoning"].get<std::string>()});
}

}  // namespace detail

Result<ComparisonResult, CompareError> request_comparison(const decode::DecodedImage& a,
                                                           const decode::DecodedImage& b,
                                                           Provider provider,
                                                           const LocalModelConfig& local_config) {
  return detail::request_comparison_impl(a, b, provider, perform_curl_post, local_config);
}

}  // namespace pzt::core::ai
