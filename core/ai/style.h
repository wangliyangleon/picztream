#pragma once

#include <string>
#include <vector>

#include "core/ai/ai.h"
#include "core/decode/decode.h"
#include "core/result.h"

// 目标三：Agent Style Stage 用的"看图选风格"AI 任务，core::ai::request_json
// 通用层的第二个消费者(第一个是 core/ai/evaluation.h)。跟 evaluation.h 同
// 一个模式，不共用同一个文件——evaluation.h 明确限定在曝光/构图/对焦三
// 维打分的 schema 上，风格建议是完全不同的 schema。见
// docs/W2026-07-15_AgentStyle_Eng_Design.md。
namespace pzt::core::ai {

enum class StyleError { MissingApiKey, NetworkError, HttpError, ParseError, Hallucinated };

struct StyleSuggestion {
  std::string recipe_name;
  std::string reasoning;
};

// preset_names：候选预设名单，由调用方(headless 命令)传入——core/ai 不碰
// DB，过滤掉 Origin 这类"不算风格"的选项是调用方的责任，不是这一层的。
// 解析出的 recipe_name 如果不在这个列表里(模型编造了一个不存在的名字)，
// 返回 StyleError::Hallucinated，不管 provider 是哪个——Provider::Local
// 会额外用 JSON Schema 的 enum 做结构化约束，但 Claude/Gemini 没有这层
// 硬约束，解析后的校验对三个 provider 一视同仁。
Result<StyleSuggestion, StyleError> request_style_suggestion(
    const decode::DecodedImage& image, const std::vector<std::string>& preset_names,
    Provider provider, const LocalModelConfig& local_config = LocalModelConfig{});

// 仅供单元测试使用——http_post 可注入，不需要真的连网络。放在 detail 里
// 是为了在头文件层面标出"这不是主 API，是给测试开的后门"，照抄
// core/ai/evaluation.h 的先例。
namespace detail {

Result<StyleSuggestion, StyleError> request_style_suggestion_impl(
    const decode::DecodedImage& image, const std::vector<std::string>& preset_names,
    Provider provider, HttpPostFn http_post,
    const LocalModelConfig& local_config = LocalModelConfig{});

}  // namespace detail

}  // namespace pzt::core::ai
