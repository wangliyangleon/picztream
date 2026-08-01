#pragma once

#include <string>
#include <vector>

#include "core/ai/ai.h"
#include "core/result.h"

// 2026-08 意图驱动跨簇选片增量：读着预选集里每张照片的**文字描述**，一次
// 调用连选带排 - 挑出用户要的 N 张并决定它们的先后顺序。core::ai::
// request_json 的第四个消费者(前三个是 evaluation.h / style.h / compare.h)，
// 但走的是新开的**纯文本**重载：这一层不看像素，输入是已经落库的描述。
//
// 不看图却仍然放在 core，判据是"输入里有没有照片信息(含照片的衍生描述)"
// 而不是"看不看像素"，取舍与被否掉的替代方案见
// docs/adr/0001-core-hosts-photo-reasoning-even-when-text-only.md。
//
// 唯一的消费者是 core::curate::curate（PRD 决策九：整条流程封在
// `pzt curate` 内部，预选集这个中间产物不暴露给 agent 搬运）。
namespace pzt::core::ai {

enum class SelectionError { MissingApiKey, NetworkError, HttpError, ParseError };

// 一张候选照片给模型看的全部材料。两个字段一起给（PRD 决策六）：只给
// content 会丢掉质量维度，而预选集里若干张都符合题材偏好时，决定选谁的恰
// 恰是 assessment。字段来源是 image_evaluations 里已有的评估记录，评估失
// 败的那几张这里是空串（模型照样能选，只是那几张没有依据）。
struct SelectionCandidate {
  std::string assessment;  // 拍得怎么样
  std::string content;     // 画面里是什么
};

// picks 是模型返回的 1-based 序号，**原样交还、不做任何清洗**。越界剔除、
// 去重、以及"有效序号不足 N 时整体退化"是调用方的判定(PRD 决策十三)，摘
// 在 core::curate::detail::resolve_selection 那个纯函数里，好让那块最容易
// 写错的逻辑不需要网络就能表驱动穷举。
struct SelectionResult {
  std::vector<int> picks;
};

// candidates 按序编成 1..K 交给模型，count 是要选几张。
//
// **选与排是同一次决定，不拆成两步**（PRD 决策十四）："开头结尾要呼应"这
// 类叙事要求会反过来影响选哪几张，拆成"先选再排"会切在错误的地方。返回顺
// 序即交付顺序。
//
// **模型只吐序号，不吐文件路径**（PRD 决策十三）：幻觉面从任意字符串缩成
// "整数在不在范围内"，Provider::Local 还能用约束解码直接卡住类型。
//
// selection_brief：用户这次想要什么(用途 / 题材偏好 / 叙事结构 提炼成的一
// 段自由文本，PRD 决策二)。为空时整段不进提示词。当前恒为空-把它从
// agent 的意图解析一路穿到这里是票 08 的事，这里先留出位置，免得那一票再
// 动一次签名与提示词。
Result<SelectionResult, SelectionError> request_selection(
    const std::vector<SelectionCandidate>& candidates, int count, Provider provider,
    const std::string& selection_brief = "",
    const LocalModelConfig& local_config = LocalModelConfig{});

// 仅供单元测试使用-http_post 可注入，不需要真的连网络。放在 detail 里是
// 为了在头文件层面标出"这不是主 API，是给测试开的后门"，照抄 compare.h/
// evaluation.h/style.h 的先例。
//
// 本模块的**成功路径**（提示词形状、序号解析、约束解码 schema）全部只能这
// 样测：curate 现有的 AI 用例一律是"让调用必然失败、只验证退化"，照抄等于
// 零覆盖（PRD 测试决策的现状警告）。
namespace detail {

Result<SelectionResult, SelectionError> request_selection_impl(
    const std::vector<SelectionCandidate>& candidates, int count, Provider provider,
    HttpPostFn http_post, const std::string& selection_brief = "",
    const LocalModelConfig& local_config = LocalModelConfig{});

}  // namespace detail

}  // namespace pzt::core::ai
