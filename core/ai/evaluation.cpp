#include "core/ai/evaluation.h"

namespace pzt::core::ai {

namespace {

// 发给 AI 的系统层框架指令——固定英文，不跟着 cli::i18n 走。W2026-07-21：
// 从"曝光/构图/对焦三维技术打分"改成"一段客观文字 assessment(覆盖构图/色
// 彩/对焦/摄影审美) + 一个 unusable 硬伤 flag"。assessment 的输出语言**始
// 终用 language 指定的语言**(cli 按当前界面语言映射后传进来)，不跟随
// extra_guidance——不给模型加"按输入语言切换"的负担；extra_guidance 只当额
// 外内容提示，不影响输出语言。框架文案本身仍是英文。
std::string build_evaluation_prompt(const std::string& extra_guidance, Language language) {
  const char* lang_word = language == Language::Chinese ? "Chinese" : "English";
  std::string prompt =
      "Assess this photo for culling. Write one concise, objective 'assessment' covering "
      "composition, color, focus, and photographic aesthetics. Also decide 'unusable': "
      "true only if the photo has a fatal flaw that makes it unusable (e.g. the subject "
      "is badly out of focus, or it is severely over/under-exposed with no recoverable "
      "detail); otherwise false. Write the assessment in ";
  prompt += lang_word;
  prompt += ".";
  if (!extra_guidance.empty()) {
    prompt += "\n\nAdditional guidance: " + extra_guidance;
  }
  return prompt;
}

std::string build_evaluation_schema_instruction() {
  return "Return a JSON object with this exact shape: "
         "{\"assessment\": <a concise objective description of the photo covering "
         "composition, color, focus, and aesthetics>, "
         "\"unusable\": <boolean, true only if the photo has a fatal flaw making it "
         "unusable>}.";
}

nlohmann::json build_evaluation_json_schema() {
  static const char* kSchemaJson = R"json({
    "type": "object",
    "properties": {
      "assessment": {"type": "string"},
      "unusable": {"type": "boolean"}
    },
    "required": ["assessment", "unusable"]
  })json";
  return nlohmann::json::parse(kSchemaJson);
}

EvaluationError map_request_error(RequestError error) {
  switch (error) {
    case RequestError::MissingApiKey:
      return EvaluationError::MissingApiKey;
    case RequestError::NetworkError:
      return EvaluationError::NetworkError;
    case RequestError::HttpError:
      return EvaluationError::HttpError;
    case RequestError::ParseError:
      return EvaluationError::ParseError;
  }
  return EvaluationError::ParseError;  // 不可达，安抚 -Wreturn-type
}

}  // namespace

namespace detail {

Result<EvaluationResult, EvaluationError> request_evaluation_impl(const decode::DecodedImage& image,
                                                                    const std::string& extra_guidance,
                                                                    Provider provider,
                                                                    HttpPostFn http_post,
                                                                    Language language,
                                                                    const LocalModelConfig& local_config) {
  std::string user_prompt = build_evaluation_prompt(extra_guidance, language);
  std::string schema_instruction = build_evaluation_schema_instruction();

  auto json_result = request_json(image, user_prompt, schema_instruction, provider, http_post, local_config,
                                   build_evaluation_json_schema());
  if (!json_result.ok()) {
    return Result<EvaluationResult, EvaluationError>::Err(map_request_error(json_result.error()));
  }

  const auto& j = json_result.value();
  if (!j.contains("assessment") || !j["assessment"].is_string() || !j.contains("unusable") ||
      !j["unusable"].is_boolean()) {
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::ParseError);
  }

  EvaluationResult result;
  result.assessment = j["assessment"].get<std::string>();
  result.unusable = j["unusable"].get<bool>();

  return Result<EvaluationResult, EvaluationError>::Ok(std::move(result));
}

}  // namespace detail

Result<EvaluationResult, EvaluationError> request_evaluation(const decode::DecodedImage& image,
                                                               const std::string& extra_guidance,
                                                               Provider provider,
                                                               Language language,
                                                               const LocalModelConfig& local_config) {
  return detail::request_evaluation_impl(image, extra_guidance, provider, perform_curl_post, language,
                                          local_config);
}

}  // namespace pzt::core::ai
