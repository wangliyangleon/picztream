#include "core/ai/evaluation.h"

#include <cmath>

namespace pzt::core::ai {

namespace {

// 固定的评价维度指令——这是发给 AI 的系统层指令，不会展示给用户看，固定
// 用英文，不跟着 cli::i18n 的 zh/en 走。用户的 extra_guidance 本身可以
// 是任何语言，模型能处理，只是包住它的框架文案是英文。明确排除色彩/情
// 绪表达这类风格判断——这是这次相对上一版"审美评分"最核心的立意变化，
// 见 docs/M3_PRD.md"背景"一节。
std::string build_evaluation_prompt(const std::string& extra_guidance) {
  std::string prompt =
      "Evaluate this photo's technical quality across three independent dimensions: "
      "exposure, composition, and focus. Judge only technical correctness - do NOT "
      "evaluate color grading, mood, or artistic style, those are out of scope.";
  if (!extra_guidance.empty()) {
    prompt += "\n\nAdditional guidance: " + extra_guidance;
  }
  return prompt;
}

std::string build_evaluation_schema_instruction() {
  return "Return a JSON object with this exact shape: "
         "{\"exposure\": {\"score\": <integer 0-10>, \"note\": <short reason>, "
         "\"fix_percent\": <optional number, suggested overall brightness adjustment in "
         "percent, omit this field entirely if no fix is needed>}, "
         "\"composition\": {\"score\": <integer 0-10>, \"note\": <short reason>, "
         "\"fix\": {\"rotate_degrees\": <number>, \"crop_left_percent\": <number>, "
         "\"crop_right_percent\": <number>, \"crop_top_percent\": <number>, "
         "\"crop_bottom_percent\": <number>} (optional object, omit entirely if no fix is "
         "needed)}, "
         "\"focus\": {\"score\": <integer 0-10>, \"note\": <short reason>}, "
         "\"comment\": <one short sentence, no more than 50 characters, summarizing all three "
         "dimensions together>}. "
         "focus never has a fix field - out-of-focus shots cannot be corrected after the "
         "fact.";
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

// score/note 是驱动达标判断/排序的核心字段，缺失或者类型不对直接判整体
// 失败；fix 相关字段单独在下面的 parse_exposure_fix/parse_composition_fix
// 里用更宽松的处理(见那两个函数的注释)，不在这里处理。
Result<DimensionAssessment, EvaluationError> parse_dimension(const nlohmann::json& dimension) {
  if (!dimension.contains("score") || !dimension["score"].is_number_integer() ||
      !dimension.contains("note") || !dimension["note"].is_string()) {
    return Result<DimensionAssessment, EvaluationError>::Err(EvaluationError::ParseError);
  }
  int score = dimension["score"].get<int>();
  if (score < 0 || score > 10) {
    return Result<DimensionAssessment, EvaluationError>::Err(EvaluationError::OutOfRange);
  }
  return Result<DimensionAssessment, EvaluationError>::Ok(
      DimensionAssessment{score, dimension["note"].get<std::string>()});
}

// 修正建议是"仅供参考"的补充信息，不影响达标判断/排序这些核心逻辑(见
// docs/M3_Eng_Design.md"风险与待确认问题"——这次没有把握模型给出的旋
// 转角度/裁切百分比这类精确几何量有多可靠)。这里故意比 parse_dimension
// 宽松：字段缺失是预期情况(模型判断不需要修正建议)，字段存在但类型不对
// 也只是当作"没有给"处理，不会因为这一个次要字段拖累整条评估结果作废。
std::optional<ExposureFix> parse_exposure_fix(const nlohmann::json& exposure) {
  if (!exposure.contains("fix_percent") || !exposure["fix_percent"].is_number()) {
    return std::nullopt;
  }
  return ExposureFix{exposure["fix_percent"].get<double>()};
}

std::optional<CompositionFix> parse_composition_fix(const nlohmann::json& composition) {
  if (!composition.contains("fix") || !composition["fix"].is_object()) return std::nullopt;
  const auto& fix = composition["fix"];

  auto get_number = [&](const char* key) -> std::optional<double> {
    if (!fix.contains(key) || !fix[key].is_number()) return std::nullopt;
    return fix[key].get<double>();
  };
  auto rotate_degrees = get_number("rotate_degrees");
  auto crop_left = get_number("crop_left_percent");
  auto crop_right = get_number("crop_right_percent");
  auto crop_top = get_number("crop_top_percent");
  auto crop_bottom = get_number("crop_bottom_percent");
  // 五个子字段同生共死——CompositionFix 是一个整体概念，缺任何一个都不
  // 算一份完整的修正建议，整体当作"没有给"处理，不拼一个残缺的建议。
  if (!rotate_degrees || !crop_left || !crop_right || !crop_top || !crop_bottom) {
    return std::nullopt;
  }
  return CompositionFix{*rotate_degrees, *crop_left, *crop_right, *crop_top, *crop_bottom};
}

}  // namespace

int overall_score(const EvaluationInfo& info) {
  double average =
      (info.exposure.score + info.composition.score + info.focus.score) / 3.0;
  return static_cast<int>(std::lround(average));
}

bool passes_gate(const EvaluationInfo& info) {
  return info.exposure.score >= kEvaluationGateThreshold &&
         info.composition.score >= kEvaluationGateThreshold &&
         info.focus.score >= kEvaluationGateThreshold;
}

namespace detail {

Result<EvaluationResult, EvaluationError> request_evaluation_impl(const decode::DecodedImage& image,
                                                                    const std::string& extra_guidance,
                                                                    Provider provider,
                                                                    HttpPostFn http_post,
                                                                    const LocalModelConfig& local_config) {
  std::string user_prompt = build_evaluation_prompt(extra_guidance);
  std::string schema_instruction = build_evaluation_schema_instruction();

  auto json_result = request_json(image, user_prompt, schema_instruction, provider, http_post, local_config);
  if (!json_result.ok()) {
    return Result<EvaluationResult, EvaluationError>::Err(map_request_error(json_result.error()));
  }

  const auto& j = json_result.value();
  if (!j.contains("exposure") || !j["exposure"].is_object() || !j.contains("composition") ||
      !j["composition"].is_object() || !j.contains("focus") || !j["focus"].is_object() ||
      !j.contains("comment") || !j["comment"].is_string()) {
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::ParseError);
  }

  auto exposure = parse_dimension(j["exposure"]);
  if (!exposure.ok()) return Result<EvaluationResult, EvaluationError>::Err(exposure.error());
  auto composition = parse_dimension(j["composition"]);
  if (!composition.ok()) return Result<EvaluationResult, EvaluationError>::Err(composition.error());
  auto focus = parse_dimension(j["focus"]);
  if (!focus.ok()) return Result<EvaluationResult, EvaluationError>::Err(focus.error());

  EvaluationResult result;
  result.exposure = exposure.value();
  result.exposure_fix = parse_exposure_fix(j["exposure"]);
  result.composition = composition.value();
  result.composition_fix = parse_composition_fix(j["composition"]);
  result.focus = focus.value();
  result.comment = j["comment"].get<std::string>();

  return Result<EvaluationResult, EvaluationError>::Ok(std::move(result));
}

}  // namespace detail

Result<EvaluationResult, EvaluationError> request_evaluation(const decode::DecodedImage& image,
                                                               const std::string& extra_guidance,
                                                               Provider provider,
                                                               const LocalModelConfig& local_config) {
  return detail::request_evaluation_impl(image, extra_guidance, provider, perform_curl_post, local_config);
}

}  // namespace pzt::core::ai
