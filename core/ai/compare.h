#pragma once

#include <string>

#include "core/ai/ai.h"
#include "core/decode/decode.h"
#include "core/result.h"

// W2026-07-21：pairwise 视觉比较——把两张图放进同一次 AI 会话，返回哪张
// 更好。core::ai::request_json 通用层的第三个消费者(前两个是 evaluation.h /
// style.h)，跟它们同一个模式：新文件、新 schema、新结果结构体，调用同一个
// request_json(这次走多图重载)。涉及质量比较的选择(dedup 保留哪张、curate
// 选哪几张)走锦标赛，pairwise 是锦标赛的一场比较——bracket 编排在 agent，
// 这一层只负责"发两张图、返回胜者"。见 docs/W2026-07-21_Eval_Eng_Design.md。
namespace pzt::core::ai {

enum class CompareError { MissingApiKey, NetworkError, HttpError, ParseError, InvalidWinner };

struct ComparisonResult {
  int winner;              // 0 => a 更好, 1 => b 更好
  std::string reasoning;   // 一句简短理由
};

// a、b 两张图放进同一次会话做视觉比较，从构图/色彩/摄影审美综合判断哪张更
// 好，必须二选一(禁止平局，bracket 需要确定晋级)。winner 非 "a"/"b" 时返回
// CompareError::InvalidWinner——跟 style.h 的 Hallucinated 同一个幻觉保护
// 思路。Provider::Local 会额外用 JSON Schema 的 enum 硬约束 winner，但解析
// 后的校验对三个 provider 一视同仁。
Result<ComparisonResult, CompareError> request_comparison(
    const decode::DecodedImage& a, const decode::DecodedImage& b, Provider provider,
    const LocalModelConfig& local_config = LocalModelConfig{});

// 仅供单元测试使用——http_post 可注入，不需要真的连网络。放在 detail 里是
// 为了在头文件层面标出"这不是主 API，是给测试开的后门"，照抄 style.h/
// evaluation.h 的先例。
namespace detail {

Result<ComparisonResult, CompareError> request_comparison_impl(
    const decode::DecodedImage& a, const decode::DecodedImage& b, Provider provider,
    HttpPostFn http_post, const LocalModelConfig& local_config = LocalModelConfig{});

}  // namespace detail

}  // namespace pzt::core::ai
