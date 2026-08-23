#pragma once

#include <string>

#include "core/ai/evaluation.h"
#include "core/db/database.h"
#include "core/project/project.h"

// image_evaluations 的唯一写入口。
//
// 两个写入方：EvaluationWorker(异步队列)与 curate(在一次 headless 调用内
// 部同步评估预选集，用不了那套队列)。语句只能有一份 - 复制一份意味着
// result_json 的形状将来要在两个地方同时改，而给这张表加字段是会反复发生
// 的事。
namespace pzt::core::ai {

// 把一次成功的评估结果写进 image_evaluations(按 image_id upsert)，并在
// auto_reject 且模型判了 unusable 时打上废片标签。
//
// extra_guidance/provider 是 provenance：模型没返回它们，是发起请求时调用
// 方就知道的上下文，跟着结果一起落库。curate 那条路径不注入用途(PRD 决策
// 五：描述保持用途中立，否则缓存只对一种用途有效)，传空串。
//
// 失败只有一种：写库本身没成功(磁盘满、库损坏)，映射成 StorageFailed。
// 不抛异常 - worker 那边跑在后台 jthread 上，未捕获异常会 std::terminate。
Result<void, EvaluationError> store_evaluation(db::Database& db, project::ImageId image_id,
                                                const EvaluationResult& result,
                                                const std::string& extra_guidance, Provider provider,
                                                bool auto_reject);

}  // namespace pzt::core::ai
