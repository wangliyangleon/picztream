#pragma once

#include <string>

#include "core/ai/ai.h"
#include "core/decode/decode.h"
#include "core/result.h"

// 审美评分——core::ai::request_json 通用层的第一个消费者。这一层知道要
// 从结果里取哪个字段、失败了怎么算，通用层不知道。见
// docs/M3_Eng_Design.md"core/api 接口设计"一节。
namespace pzt::core::ai {

enum class ScoreError { MissingApiKey, NetworkError, HttpError, ParseError, OutOfRange };

struct ScoreResult {
  int score;            // 1-100
  std::string comment;  // 简短点评（英文，见 score.cpp 的说明）
};

// 公开入口——签名精确匹配 core/ai/score_worker.h(下一个 increment)里
// ScoreWorker::ScoreFn 的类型，可以直接当默认值用。内部固定走真实的
// curl 实现，不接受注入。
Result<ScoreResult, ScoreError> request_score(const decode::DecodedImage& image,
                                               const std::string& extra_guidance,
                                               Provider provider);

// 仅供单元测试使用——http_post 可注入，不需要真的连网络就能验证 prompt
// 拼接、字段提取、越界校验这些逻辑；上面的 request_score 是这个函数在
// 默认 http_post 参数下的一层薄封装。放在 detail 里是为了在头文件层面
// 标出"这不是主 API，是给测试开的后门"，不是漏了整理——它不能直接当
// ScoreFn 的默认值用（4 个参数，ScoreFn 期望 3 个，函数指针不能隐式丢
// 掉参数）。
namespace detail {

Result<ScoreResult, ScoreError> request_score_impl(const decode::DecodedImage& image,
                                                     const std::string& extra_guidance,
                                                     Provider provider, HttpPostFn http_post);

}  // namespace detail

}  // namespace pzt::core::ai
