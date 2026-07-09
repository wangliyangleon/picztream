#include "core/ai/score.h"

namespace pzt::core::ai {

namespace {

// 固定的评价维度指令——这是发给 AI 的系统层指令，不会展示给用户看，固定
// 用英文，不跟着 cli::i18n 的 zh/en 走。用户的 extra_guidance 本身可以
// 是任何语言，模型能处理，只是包住它的框架文案是英文。
std::string build_evaluation_prompt(const std::string& extra_guidance) {
  std::string prompt =
      "Evaluate the aesthetic quality of this photo. Consider dimensions such as color, "
      "composition, lighting, and overall mood.";
  if (!extra_guidance.empty()) {
    prompt += "\n\nAdditional guidance: " + extra_guidance;
  }
  return prompt;
}

std::string build_score_schema_instruction() {
  return "Return a JSON object with exactly two fields: \"score\" (an integer from 1 to 100) "
         "and \"comment\" (a short comment, no more than 50 characters, capturing your overall "
         "impression).";
}

ScoreError map_request_error(RequestError error) {
  switch (error) {
    case RequestError::MissingApiKey:
      return ScoreError::MissingApiKey;
    case RequestError::NetworkError:
      return ScoreError::NetworkError;
    case RequestError::HttpError:
      return ScoreError::HttpError;
    case RequestError::ParseError:
      return ScoreError::ParseError;
  }
  return ScoreError::ParseError;  // 不可达，安抚 -Wreturn-type
}

}  // namespace

namespace detail {

Result<ScoreResult, ScoreError> request_score_impl(const decode::DecodedImage& image,
                                                     const std::string& extra_guidance,
                                                     Provider provider, HttpPostFn http_post) {
  std::string user_prompt = build_evaluation_prompt(extra_guidance);
  std::string schema_instruction = build_score_schema_instruction();

  auto json_result = request_json(image, user_prompt, schema_instruction, provider, http_post);
  if (!json_result.ok()) {
    return Result<ScoreResult, ScoreError>::Err(map_request_error(json_result.error()));
  }

  const auto& j = json_result.value();
  if (!j.contains("score") || !j["score"].is_number_integer() || !j.contains("comment") ||
      !j["comment"].is_string()) {
    return Result<ScoreResult, ScoreError>::Err(ScoreError::ParseError);
  }

  int score = j["score"].get<int>();
  if (score < 1 || score > 100) {
    return Result<ScoreResult, ScoreError>::Err(ScoreError::OutOfRange);
  }

  return Result<ScoreResult, ScoreError>::Ok(ScoreResult{score, j["comment"].get<std::string>()});
}

}  // namespace detail

Result<ScoreResult, ScoreError> request_score(const decode::DecodedImage& image,
                                               const std::string& extra_guidance,
                                               Provider provider) {
  return detail::request_score_impl(image, extra_guidance, provider, perform_curl_post);
}

}  // namespace pzt::core::ai
