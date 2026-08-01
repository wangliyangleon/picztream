#include "core/ai/evaluation.h"

namespace pzt::core::ai {

namespace {

// 发给 AI 的系统层框架指令——固定英文，不跟着 cli::i18n 走。W2026-07-21：
// 从"曝光/构图/对焦三维技术打分"改成"一段客观文字 assessment(覆盖构图/色
// 彩/对焦/摄影审美) + 一个 unusable 硬伤 flag"。assessment 的输出语言**始
// 终用 language 指定的语言**(cli 按当前界面语言映射后传进来)，不跟随
// extra_guidance——不给模型加"按输入语言切换"的负担；extra_guidance 只当额
// 外内容提示，不影响输出语言。框架文案本身仍是英文。
// 2026-07-22 修订：真机测试发现一张明显失焦、构图无意义的"膝盖照"被判成
// usable——旧措辞"badly out of focus"+"severely...with no recoverable
// detail"把门槛设得太高，只有灾难级模糊/曝光才会命中。改成两点：(1) 对焦
// 判据从"badly"（要求极端模糊）降到"不足以当作清晰可用的照片"，不再要求
// 灾难级；(2) 补一条"意外/无意义构图"（没有可辨识主体，比如误拍身体部
// 位/口袋/地面），覆盖"膝盖照"这类technically 没爆炸但根本不构成一张照片
// 的情况——这仍然是"硬伤"范畴（PRD 定义的技术/实用性判据），不是转向宽
// 泛的审美评分，没打算让 unusable 变成"不够好看就拒"。
std::string build_evaluation_prompt(const std::string& extra_guidance, Language language) {
  const char* lang_word = language == Language::Chinese ? "Chinese" : "English";
  std::string prompt =
      "Assess this photo for culling. Write one concise, objective 'assessment' covering "
      "composition, color, focus, and photographic aesthetics. Also decide 'unusable': "
      "true if the photo has a fatal flaw that makes it unfit to keep -- the main "
      "subject is out of focus or motion-blurred enough that it isn't a sharp, usable "
      "shot, or the exposure is severely blown out or crushed with no recoverable "
      "detail, or the frame is an accidental/pointless capture with no coherent "
      "photographic subject (e.g. a stray shot of a body part, pocket, or the ground). "
      "Judge focus and framing strictly: a photo does not need to be catastrophically "
      "bad to count as unusable, it only needs to fail as something worth keeping. "
      "Otherwise unusable is false. Write the assessment in ";
  prompt += lang_word;
  prompt += ".";
  // content 的措辞是本增量最需要真机调参的地方(PRD 风险二，标高风险)：不点
  // 名这四个要素、不明确禁止重复摄影评语的话，模型会顺着上面 assessment 的
  // 调子继续写"构图均衡、色彩温暖"，两个字段变成同义反复，而下游的跨簇选片
  // 和文案手里就只剩摄影评语、没有画面事实可用。
  prompt +=
      " Separately, write a 'content' description of the scene itself: what is happening, "
      "who is in the frame, where it appears to be, and what the mood is. Be concrete and "
      "specific -- name the subjects, the setting, and the action. Do not repeat the "
      "photographic critique: content is about what the photo shows, not about how well it "
      "was shot, so say nothing about composition, color, focus, or exposure. Write the "
      "content in ";
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
         "unusable>, "
         "\"content\": <what the photo shows: what is happening, who is in it, where, "
         "and the mood -- not a critique of how it was shot>}.";
}

// 描述的形状在三处被定义：上面的提示词、上面的 schema instruction，以及这里
// 的约束解码 JSON Schema。三处必须一起改 - 漏掉这一处，本地模型(Ollama 的
// format 字段)会不稳定地省略新字段，云端 provider 反而看不出问题，因为它们
// 只吃前两处的自然语言。
nlohmann::json build_evaluation_json_schema() {
  static const char* kSchemaJson = R"json({
    "type": "object",
    "properties": {
      "assessment": {"type": "string"},
      "unusable": {"type": "boolean"},
      "content": {"type": "string"}
    },
    "required": ["assessment", "unusable", "content"]
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

  // content 跟另外两个字段同等严格(见 evaluation.h 的说明)：宽松退化为空串
  // 会在"有评估记录就跳过"的缓存判据下留一条永不刷新的哑记录。
  const auto& j = json_result.value();
  if (!j.contains("assessment") || !j["assessment"].is_string() || !j.contains("unusable") ||
      !j["unusable"].is_boolean() || !j.contains("content") || !j["content"].is_string()) {
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::ParseError);
  }

  EvaluationResult result;
  result.assessment = j["assessment"].get<std::string>();
  result.unusable = j["unusable"].get<bool>();
  result.content = j["content"].get<std::string>();

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
