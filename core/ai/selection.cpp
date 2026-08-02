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

  // 票 07（PRD 决策十五）：文案搭在同一次调用上。三件事在这一段里被说死：
  //
  // 1. **写的是被选中的那几张**，不是整个预选集。模型眼前列着 K 条描述而只
  //    挑了 count 条，一句含糊的"these photos"会让它顺手把落选的那几张也写
  //    进去 - 交付的照片里没有的东西出现在文案里，用户当场就能发现。
  // 2. **材料是 content 与 assessment 两栏，但落笔写的是画面**（决策十五、
  //    六）。这是照着风险二写的：模型手里只有描述、没有照片，不点破的话它
  //    会顺着 quality 那一栏的调子继续写摄影评语("光影层次丰富、构图考究")，
  //    那是给摄影师看的话，不是能发出去的话。所以两栏都留在上下文里(选片本
  //    来就要用 assessment)，只约束成品不许点评拍摄手法。
  // 3. **语言跟着描述走，不跟界面语言走**：core 不认识 cli 的界面语言(i18n
  //    只在 cli 层)，而描述是评估那一步用界面无关的固定语言写的，让文案随它
  //    就自动落在同一种语言上，比给 core 接一个语言参数少一处会走偏的地方。
  prompt +=
      "\n\nAlso write a 'caption' the user can post as-is alongside the photos you picked "
      "-- it is about those, not about the ones you left out. Say what is happening in "
      "them, drawing on their 'content' notes; their 'quality' notes tell you how well each "
      "one came out, so let those steer which moment you lead with, but never critique or "
      "mention the photography itself -- this is the user's own post, not a review. Keep it "
      "to one or two sentences, and write it in the same language as the notes above.";

  // 票 08 之前恒为空：为空时整段省略，而不是留一句空的"用户要求：",那会让
  // 模型去揣摩一个不存在的要求。
  if (!selection_brief.empty()) {
    // 票 07：同一段简述同时驱动两件事。用途("发朋友圈"/"给家人看")已经在里
    // 面了，文案的语气与平台适配因此不需要新的输入(PRD 决策十五)。
    prompt +=
        "\n\nThe user asked for this, in their own words -- let it drive both which photos "
        "you pick and the order you put them in, and let it set the tone of the caption: " +
        selection_brief;
  }
  return prompt;
}

std::string build_selection_schema_instruction(int count) {
  return "Return a JSON object with this exact shape: {\"picks\": [<the " +
         std::to_string(count) +
         " chosen photo numbers, in presentation order>], \"caption\": \"<the caption>\"}. "
         "Every element of picks must be one of the photo numbers listed above.";
}

// 约束解码用的 JSON Schema-形状在三处被定义(提示词、schema instruction、
// 这里)，必须一起改。漏掉这一处，本地模型会不稳定地吐回字符串序号或者别的
// 键名，而云端 provider 反而看不出问题，因为它们只吃前两处的自然语言。同
// evaluation.cpp 的说明。
//
// 票 07：caption 进 properties 但**不进 required**，这就是决策十五说的"在返
// 回 schema 里是可选字段"落到 schema 层的样子。进 required 会让本地模型在写
// 不出文案时被约束解码逼着编一段，或者让整个响应作废、连 picks 一起丢-两
// 者都恰好是失败隔离要防的事。
nlohmann::json build_selection_json_schema() {
  static const char* kSchemaJson = R"json({
    "type": "object",
    "properties": {
      "picks": {"type": "array", "items": {"type": "integer"}},
      "caption": {"type": "string"}
    },
    "required": ["picks"]
  })json";
  return nlohmann::json::parse(kSchemaJson);
}

// 文案两端的空白裁掉。约束解码卡得住类型，卡不住"是不是一段能发出去的话"：
// 只有空白的文案跟没给是同一回事，不该让 agent 把一条空消息发给用户。
std::string trim_caption(const std::string& raw) {
  const char* kSpace = " \t\r\n";
  auto begin = raw.find_first_not_of(kSpace);
  if (begin == std::string::npos) return "";
  auto end = raw.find_last_not_of(kSpace);
  return raw.substr(begin, end - begin + 1);
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

  // 票 07（PRD 决策十五）：文案缺失或不合法**只丢文案**，不报错、不重试。
  // 这里刻意没有 else 分支去记一个"文案失败"的信号-对每一个下游而言"没有
  // 文案"只有一种处置(不展示)，具体是模型没给还是给歪了不改变这个决定。
  if (j.contains("caption") && j["caption"].is_string()) {
    result.caption = trim_caption(j["caption"].get<std::string>());
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
