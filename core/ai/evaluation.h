#pragma once

#include <optional>
#include <string>

#include "core/ai/ai.h"
#include "core/decode/decode.h"
#include "core/result.h"

// 选片辅助评估——core::ai::request_json 通用层的消费者。曝光、构图、对焦
// 三个技术维度各自打分，不涉及色彩、情绪表达这类风格判断，见
// docs/M3_PRD.md/M3_Eng_Design.md。取代了 M3 增量一最初那版"审美评分"
// （core/ai/score.h，已删除）。
namespace pzt::core::ai {

enum class EvaluationError { MissingApiKey, NetworkError, HttpError, ParseError, OutOfRange };

struct DimensionAssessment {
  int score;  // 0-10
  std::string note;
};

struct ExposureFix {
  double adjust_percent;
};

struct CompositionFix {
  double rotate_degrees;
  double crop_left_percent;
  double crop_right_percent;
  double crop_top_percent;
  double crop_bottom_percent;
};

// 纯模型输出——不含 extra_guidance/provider，那两个不是模型返回的，是发
// 起请求时调用方已经知道的上下文。exposure_fix/composition_fix 允许
// nullopt：模型判断分数已经够高、不需要修正建议时就不给这个字段，不强
// 迫它硬凑一个"建议调整 0%"。focus 没有对应的 fix 字段——对焦软了没有
// 后期能修的办法，这次不问模型要。
struct EvaluationResult {
  DimensionAssessment exposure;
  std::optional<ExposureFix> exposure_fix;
  DimensionAssessment composition;
  std::optional<CompositionFix> composition_fix;
  DimensionAssessment focus;
  std::string comment;  // 跨三项的一句总体归纳，不是某一项的 note
};

// 落库/读回用的完整形状——比 EvaluationResult 多两个字段(extra_guidance/
// provider)，见 core/project/project.h 的 ImageInfo::evaluation。
struct EvaluationInfo {
  DimensionAssessment exposure;
  std::optional<ExposureFix> exposure_fix;
  DimensionAssessment composition;
  std::optional<CompositionFix> composition_fix;
  DimensionAssessment focus;
  std::string comment;
  std::string extra_guidance;
  std::string provider;
};

// 综合分数(三项平均、四舍五入)和是否达标(三项都 >= kEvaluationGateThreshold)
// 不入库——完全由三个维度的分数算出来，存成列反而多一条"存出来的值跟三
// 项分数对不上"的风险(比如以后调整阈值，历史行的达标状态还留着按旧阈值
// 算出来的值)。这两个函数是这份契约的唯一实现，CLI 展示和以后近似重复
// 检测排序都调这两个函数，不会出现两处各自算一遍、算法不一致的风险。
constexpr int kEvaluationGateThreshold = 6;

int overall_score(const EvaluationInfo& info);
bool passes_gate(const EvaluationInfo& info);

// extra_guidance:用户在 `:` 里输入的原始文本，可能是空字符串。内部拼出
// 完整的评估任务描述——固定模板(要求模型从曝光、构图、对焦三个技术维度
// 分别评估，不涉及色彩、情绪表达这类风格判断)后面跟一段"Additional
// guidance: {extra_guidance}"(为空时省略)，作为 user_prompt 传给
// request_json；schema_instruction 描述 EvaluationResult 对应的 JSON 形
// 状。从结果 JSON 里取值、校验三个 score 字段都落在 0-10——任何一个取不
// 到、类型不对、或者越界，整体算失败，不写库(EvaluationError::
// OutOfRange)。RequestError 直接映射到同名的 EvaluationError。模板本身
// 不会展示给用户看，固定用英文，不跟着 cli::i18n 走(用户的额外指引本身
// 可以是任何语言)。
//
// 签名精确匹配 core/ai/evaluation_worker.h 里 EvaluationWorker::
// EvaluationFn 的类型(3 个参数)，可以直接当默认值用。
Result<EvaluationResult, EvaluationError> request_evaluation(const decode::DecodedImage& image,
                                                               const std::string& extra_guidance,
                                                               Provider provider);

// 仅供单元测试使用——http_post 可注入，不需要真的连网络就能验证 prompt
// 拼接、字段提取、越界校验这些逻辑；上面的 request_evaluation 是这个函
// 数在默认 http_post 参数下的一层薄封装。放在 detail 里是为了在头文件层
// 面标出"这不是主 API，是给测试开的后门"——它不能直接当 EvaluationFn 的
// 默认值用(4 个参数，EvaluationFn 期望 3 个)。
namespace detail {

Result<EvaluationResult, EvaluationError> request_evaluation_impl(const decode::DecodedImage& image,
                                                                    const std::string& extra_guidance,
                                                                    Provider provider,
                                                                    HttpPostFn http_post);

}  // namespace detail

}  // namespace pzt::core::ai
