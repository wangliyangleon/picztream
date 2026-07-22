#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/ai/ai.h"
#include "core/ai/compare.h"
#include "core/db/database.h"
#include "core/dedup/dedup.h"
#include "core/decode/decode.h"
#include "core/project/project.h"
#include "core/result.h"

// W2026-07-21 目标二：dedup 留哪张/curate 每簇选哪张，这两件"涉及比较的
// 选择"收口成同一个函数的两个分支——分簇本身复用 core::dedup::
// find_duplicates 的现成算法，AI 关时选 winner 复用 dedup 已经算好的
// keep_id(见 dedup.h pick_keep_id 的说明)，AI 开时才真的两两送进
// core::ai::request_comparison 跑单淘汰锦标赛。bracket 推进(谁跟谁比、如
// 何晋级)是纯确定性算法，不含需要 agent 判断的业务逻辑，所以整个锦标赛
// (分簇 + 场次推进 + 判定胜者)都收在这一层，一次调用做完——不是像 PRD
// 最初设想那样把 bracket 推进摆到 agent(Python)侧。见
// docs/W2026-07-21_Tournament_Eng_Design.md 决策一。
namespace pzt::core::tournament {

// 一个簇的选择结果。members 是簇内全部成员(含 winner)；size==1 是没有
// 落进任何 dHash 分组的单例——它本来就是"唯一候选"，winner 恒等于自己，
// 不发起任何比较。
struct ClusterChoice {
  std::vector<project::ImageId> members;
  project::ImageId winner;
};

struct ChooseSummary {
  // 覆盖排除标签之后的全部候选(含单例)，不只是聚类命中的那部分——curate
  // 挑最终 N 张需要完整候选池。
  std::vector<ClusterChoice> clusters;
  // apply_dup_tag=true 时被打上"重复"标签的图片数(每簇内非 winner 的成
  // 员)；apply_dup_tag=false 时恒为 0。
  int tagged_count;
  // 候选里 captured_at 为 NULL、完全没法参与时间聚类的图片数(仍然会作
  // 为单例出现在 clusters 里，这个字段只是提供可观测性)。
  int skipped_no_capture_time;
  // ai_enabled=true 时，因为某次 request_comparison 调用失败而整簇退化
  // 成"选 captured_at 最新"的簇数；ai_enabled=false 时恒为 0。
  int ai_fallback_count;
};

// image_ids 是调用方已经解析好的范围(整个项目还是某个标签的子集)。
// time_window_seconds/hash_threshold 直接传给 dedup::find_duplicates，
// dedup 用细参数(挑近乎同一张)，curate 用粗参数(挑同一场景)。
// exclude_tag_names 是要从 image_ids 里摘掉的标签名列表(dedup 传
// {"废片"}，curate 传 {"废片","重复"})，标签不存在时按"不排除任何东西"
// 处理，不是错误。apply_dup_tag=true 时对每个 size>=2 的簇的非 winner
// 成员打"重复"标签(dedup 用；curate 传 false，簇内落选不等于"重复"，
// 语义不一样)。ai_enabled=false 时每簇 winner 直接复用 dedup::
// find_duplicates 算好的 keep_id(captured_at 最新，id 兜底)；
// ai_enabled=true 时簇内单淘汰锦标赛决定 winner，某次比较失败时那一簇
// 单独退化成 keep_id、不中断其它簇(ai_fallback_count 记录退化了几簇)。
// on_progress：跟 dedup::find_duplicates 同款回调，每处理完一个候选簇
// (不论是否成簇)回调一次——直接转给内部的 find_duplicates_impl，不是新
// 概念。Commit 2 补上这个参数：find_and_tag_duplicates 今天的公开签名带
// 这个回调，改調 cluster_and_choose 时不能悄悄把它吞掉。
Result<ChooseSummary, project::ProjectNotFoundError> cluster_and_choose(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold, const std::vector<std::string>& exclude_tag_names,
    bool apply_dup_tag, bool ai_enabled, ai::Provider ai_provider = ai::Provider::Local,
    const ai::LocalModelConfig& local_config = ai::LocalModelConfig{},
    dedup::DedupProgressFn on_progress = nullptr);

// 仅供单元测试使用——decode_fn/compare_fn 都可注入，不需要真的解码 JPEG
// 或真的连网络就能验证分簇后处理、锦标赛推进、AI 失败退化这些逻辑。跟
// core/dedup/dedup.h 的 detail::find_duplicates_impl(注入 decode_fn)、
// core/ai/compare.h 的 detail::request_comparison_impl(注入 http_post)
// 是同一个模式——production 的 cluster_and_choose 就是这个函数塞真实
// decode_fn/compare_fn 的一层薄封装，不是"仅测试可调"的隔离代码。
namespace detail {

using CompareFn = std::function<Result<ai::ComparisonResult, ai::CompareError>(
    const decode::DecodedImage&, const decode::DecodedImage&, ai::Provider, const ai::LocalModelConfig&)>;

Result<ChooseSummary, project::ProjectNotFoundError> cluster_and_choose_impl(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold, const std::vector<std::string>& exclude_tag_names,
    bool apply_dup_tag, bool ai_enabled, ai::Provider ai_provider, const ai::LocalModelConfig& local_config,
    dedup::detail::PreviewDecodeFn decode_fn, CompareFn compare_fn,
    dedup::DedupProgressFn on_progress = nullptr);

}  // namespace detail

}  // namespace pzt::core::tournament
