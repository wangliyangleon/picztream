#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/ai/ai.h"
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
// docs/history/W2026-07-21_Tournament_Eng_Design.md 决策一。
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
  // on_ai_gate 返回 false(调用方看过真实开销之后不跑了)。只可能在
  // ai_enabled=true 且传了 on_ai_gate 时为真。为真时 clusters 为空、
  // tagged_count 为 0，且**一个标签都没写过**，调用方应该按"这次什么都
  // 没发生"处理，而不是把空 clusters 当成"一组重复都没有"的结论。
  bool ai_declined = false;
  // CancelFn 返回了 true。跟 ai_declined 一样：clusters 为空、
  // tagged_count 为 0、一个标签都没写过。两者分开报是因为对用户的含义不
  // 同——一个是"没点头"，一个是"点了头又喊停"。
  bool cancelled = false;
};

// AI 锦标赛真正开跑前的闸门，在本地分簇跑完、任何一次 request_comparison
// 发出之前调用一次。AiCost.group_count 是 size>=2 的簇数，comparison_count
// 是这些簇的 (members.size()-1) 之和-单淘汰赛 N 个成员恰好 N-1 场(见
// run_bracket 的说明)，所以这是精确开销而不是估算，调用方可以拿它直接问
// 用户"要不要为此发这么多次请求"。candidate_count 是候选总数(含单例)，
// 锦标赛自己用不上，是给 curate 算评估张数用的(见 dedup.h 上的说明)。
//
// 返回 false = 不跑：不发起任何比较、不写任何标签，直接返回
// ai_declined=true 的空结果。nullptr(默认)= 无条件继续，等价于加这个参数
// 之前的行为，所以 curate 和全部既有调用点不受影响。
//
// 只在 ai_enabled=true 且确实存在 size>=2 的簇时才会被调用——没有任何可
// 比较的簇时问了也没有意义(开销恒为 0)，直接当同意处理。
//
// 类型本身定义在 core/dedup/dedup.h(那边有说明为什么不能反过来)，这里
// 引进本命名空间；这两行是同一个类型的两个名字，不是两套东西。
using AiCost = dedup::AiCost;
using AiGateFn = dedup::AiGateFn;

// **每发起一次比较之前**回调一次，带上"第几组/共几组"和"第几次比较/共几
// 次比较"两级计数(字段说明见 dedup.h 的 AiProgress)。
//
// 两个要点都是踩过的坑：(1) 在比较**之前**报,不是之后——报"已完成"的话,
// 第一次网络往返结束前屏幕上一个字都不会变;(2) 报到**每次比较**,不是每
// 簇——一簇 members.size()-1 次串行调用、每次受 60s 超时约束,只报簇号时
// 单个大簇期间画面照样静止几分钟。
//
// 某簇中途失败(解码失败或 compare_fn 返回 Err)时,该簇剩下的比较不会发
// 生,但 comparison_done 会在进入下一簇时补齐到"跨过的簇应有的次数",所以
// 计数始终跟 AiGateFn 报出的总数对得上,不会停在一个够不着的数上。
//
// 跟 dedup::DedupProgressFn 数的东西完全不同——那个数的是本地分簇阶段的
// 候选簇(包括最后没成簇的)，这个只数真的发起了 AI 比较的簇，两者的 total
// 一般对不上，别把同一个回调同时传给两边。
using AiProgress = dedup::AiProgress;
using AiProgressFn = dedup::AiProgressFn;

// 中途取消(语义见 dedup.h 的 CancelFn，必须粘性)。查三处：本地分簇的每
// 张图、AI 的每次比较、两个阶段之间。查到 true 就返回 cancelled=true 的
// 空结果，一个标签都不写——写库统一在最后一步，所以这里不需要回滚。
using CancelFn = dedup::CancelFn;

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
// on_ai_gate/on_ai_progress 见上面各自的说明，都默认 nullptr(不闸不报进
// 度)，所以加这两个参数不改变任何既有调用点的行为。
Result<ChooseSummary, project::ProjectNotFoundError> cluster_and_choose(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold, const std::vector<std::string>& exclude_tag_names,
    bool apply_dup_tag, bool ai_enabled, ai::Provider ai_provider = ai::Provider::Local,
    const ai::LocalModelConfig& local_config = ai::LocalModelConfig{},
    dedup::DedupProgressFn on_progress = nullptr, AiGateFn on_ai_gate = nullptr,
    AiProgressFn on_ai_progress = nullptr, CancelFn on_cancel = nullptr);

// 仅供单元测试使用——decode_fn/compare_fn 都可注入，不需要真的解码 JPEG
// 或真的连网络就能验证分簇后处理、锦标赛推进、AI 失败退化这些逻辑。跟
// core/dedup/dedup.h 的 detail::find_duplicates_impl(注入 decode_fn)、
// core/ai/compare.h 的 detail::request_comparison_impl(注入 http_post)
// 是同一个模式——production 的 cluster_and_choose 就是这个函数塞真实
// decode_fn/compare_fn 的一层薄封装，不是"仅测试可调"的隔离代码。
namespace detail {

// 比较原语：给两张已解码的图，说赢的是左边还是右边。这是 bracket 推进对
// "一次比较"的全部要求，与这次比较由谁做出来无关 - AI 那条路是
// cluster_and_choose 把 ai::request_comparison 包成一层 adapter 注进来，
// 供应商与本地模型配置在 adapter 内部捕获，不出现在这一层的签名里。
//
// nullopt = 这次比较没有结果(解码之外的失败：网络断了、模型输出解析不出
// 胜者等)。失败的具体原因 bracket 推进用不上 - 它对每一种失败的反应完全
// 相同(这一簇整体退化)，所以这里只保留"有没有赢家"这一个信息。
enum class ComparisonWinner { Left, Right };

using CompareFn = std::function<std::optional<ComparisonWinner>(const decode::DecodedImage&,
                                                                const decode::DecodedImage&)>;

Result<ChooseSummary, project::ProjectNotFoundError> cluster_and_choose_impl(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold, const std::vector<std::string>& exclude_tag_names,
    bool apply_dup_tag, bool ai_enabled, dedup::detail::PreviewDecodeFn decode_fn, CompareFn compare_fn,
    dedup::DedupProgressFn on_progress = nullptr, AiGateFn on_ai_gate = nullptr,
    AiProgressFn on_ai_progress = nullptr, CancelFn on_cancel = nullptr);

}  // namespace detail

}  // namespace pzt::core::tournament
