#include "core/ai/selection.h"

#include <string>
#include <vector>

namespace pzt::core::ai {

namespace {

// 发给模型的任务描述-固定英文框架(同 evaluation.cpp/compare.cpp 的惯
// 例，框架文案是给模型的系统指令，不跟 cli::i18n 走)，中间嵌的是照片描述
// 与选片简述的原文，那两段本来就是用界面语言写的。
//
// 三件事在这段提示词里被说死：(1) 照片用序号标识，模型只能吐序号；(2) 选
// 与排是同一次决定，返回顺序就是最终呈现顺序；(3) 正好要 count 张。
std::string build_selection_prompt(const std::vector<SelectionCandidate>& candidates, int count,
                                    const std::string& selection_brief) {
  std::string prompt =
      "You are picking photos out of a shortlist. Each photo below is identified by its "
      "number, and comes with two things: a 'quality' note on how well it was shot, and a "
      "'content' note on what the photo actually shows. You cannot see the photos "
      "themselves -- these notes are all you get.\n\n";

  for (std::size_t i = 0; i < candidates.size(); ++i) {
    prompt += std::to_string(i + 1) + ". quality: " + candidates[i].assessment +
              " | content: " + candidates[i].content + "\n";
  }

  prompt += "\nPick exactly " + std::to_string(count) +
            " of them by number. Return the numbers in the order the photos should be "
            "presented: the order is part of the decision, not an afterthought -- the first "
            "one carries the most weight, and the sequence should read well as a set. Use "
            "only the numbers listed above, and do not repeat a number.";

  // 票 08 之前恒为空：为空时整段省略，而不是留一句空的"用户要求：",那会让
  // 模型去揣摩一个不存在的要求。
  if (!selection_brief.empty()) {
    prompt +=
        "\n\nThe user asked for this, in their own words -- let it drive both which photos "
        "you pick and the order you put them in: " +
        selection_brief;
  }
  return prompt;
}

std::string build_selection_schema_instruction(int count) {
  return "Return a JSON object with this exact shape: {\"picks\": [<the " +
         std::to_string(count) +
         " chosen photo numbers, in presentation order>]}. Every element must be one of the "
         "photo numbers listed above.";
}

// 约束解码用的 JSON Schema-形状在三处被定义(提示词、schema instruction、
// 这里)，必须一起改。漏掉这一处，本地模型会不稳定地吐回字符串序号或者别的
// 键名，而云端 provider 反而看不出问题，因为它们只吃前两处的自然语言。同
// evaluation.cpp 的说明。
nlohmann::json build_selection_json_schema() {
  static const char* kSchemaJson = R"json({
    "type": "object",
    "properties": {
      "picks": {"type": "array", "items": {"type": "integer"}}
    },
    "required": ["picks"]
  })json";
  return nlohmann::json::parse(kSchemaJson);
}

SelectionError map_request_error(RequestError error) {
  switch (error) {
    case RequestError::MissingApiKey:
      return SelectionError::MissingApiKey;
    case RequestError::NetworkError:
      return SelectionError::NetworkError;
    case RequestError::HttpError:
      return SelectionError::HttpError;
    case RequestError::ParseError:
      return SelectionError::ParseError;
  }
  return SelectionError::ParseError;  // 不可达，安抚 -Wreturn-type
}

}  // namespace

namespace detail {

Result<SelectionResult, SelectionError> request_selection_impl(
    const std::vector<SelectionCandidate>& candidates, int count, Provider provider,
    HttpPostFn http_post, const std::string& selection_brief,
    const LocalModelConfig& local_config) {
  std::string user_prompt = build_selection_prompt(candidates, count, selection_brief);
  std::string schema_instruction = build_selection_schema_instruction(count);

  auto json_result = request_json(user_prompt, schema_instruction, provider, std::move(http_post),
                                   local_config, build_selection_json_schema());
  if (!json_result.ok()) {
    return Result<SelectionResult, SelectionError>::Err(map_request_error(json_result.error()));
  }

  const auto& j = json_result.value();
  if (!j.contains("picks") || !j["picks"].is_array()) {
    return Result<SelectionResult, SelectionError>::Err(SelectionError::ParseError);
  }

  // 数组里混进非整数的元素时**跳过它、留下其余的**，不整批算失败：跟 PRD
  // 决策十三对越界序号的处置是同一个立场 - 模型返回 9 个好序号外加 1 个坏
  // 的，把整批扔掉丢的是真信号。真正的"整体退化"分界在调用方那一层，由清
  // 洗完还剩几个决定。
  SelectionResult result;
  for (const auto& entry : j["picks"]) {
    if (entry.is_number_integer()) result.picks.push_back(entry.get<int>());
  }
  return Result<SelectionResult, SelectionError>::Ok(std::move(result));
}

}  // namespace detail

Result<SelectionResult, SelectionError> request_selection(
    const std::vector<SelectionCandidate>& candidates, int count, Provider provider,
    const std::string& selection_brief, const LocalModelConfig& local_config) {
  return detail::request_selection_impl(candidates, count, provider, perform_curl_post,
                                         selection_brief, local_config);
}

}  // namespace pzt::core::ai
