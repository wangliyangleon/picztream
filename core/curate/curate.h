#pragma once

#include <optional>
#include <vector>

#include "core/ai/ai.h"
#include "core/db/database.h"
#include "core/dedup/dedup.h"  // DedupProgressFn / AiProgressFn(两者都定义在这里)
#include "core/project/project.h"
#include "core/tagging/tagging.h"

// 策展挑图算法。见 docs/history/M4_Eng_Design.md 第三节"Curate 算法设计"——从非
// 废片/非重复的候选图里挑 N 张兼顾多样性的代表作(W2026-07-21：候选不再
// 依赖是否评估过，见 core/tournament 的说明)。这里只负责选择，不打标
// 签、不导出（单一职责）；`pzt curate` 命令拿到结果后自己去调 add_tag
// 落用户指定的标签。
namespace pzt::core::curate {

struct CurateResult {
  // 有序，且这个顺序有意义：调用方(pzt curate -> agent -> Deliver)按它的
  // 次序发送，决定九宫格位置与轮播先后。票 01 起两条确定性路径(关 AI 凑够
  // count、候选簇数不足 count)都是 captured_at 降序、id 升序；开 AI 且候选
  // 够那条仍是 std::sample 的保序输出(即簇遍历顺序)，票 06 改由模型决定。
  std::vector<project::ImageId> selected;
  int requested;
  int returned;  // == selected.size()，< requested 表示候选不足
  // W2026-07-21 目标二：ai_enabled=true 时，因为某次 AI 比较失败而整簇
  // 退化成"选 captured_at 最新"的簇数；ai_enabled=false 时恒为 0。见
  // core::tournament::ChooseSummary 同名字段。
  int ai_fallback_count;
  // 票 05（PRD 决策十九，跟 on_ai_gate 同一刀偿还）：调用方在闸门上说了
  // 不跑。为真时 selected 为空、returned 为 0，且**一次视觉调用都没发
  // 出、一个字节都没写库**。
  //
  // 补这两个字段的原因是它们跟"候选池本来就是空的"折叠在了同一个返回值
  // 上：selected 空 + returned 0 此前唯一的含义是"这个项目里没有可选的
  // 照片"，agent 照着这个语义对用户说话。不分开的话，用户在闸门上点"不
  // 跑"会收到一句"没选出照片"，跟事实完全不符。
  bool ai_declined = false;
  // 票 05：on_cancel 返回了 true。跟 ai_declined 同形(selected 空、零写
  // 入)，分开报是因为对用户的含义不同——一个是"没点头"，一个是"点了头又
  // 喊停"。见 core::tournament::ChooseSummary 的同名字段。
  bool cancelled = false;
};

// 票 05：AI 真正开跑前的合并开销闸门。comparison_count 是簇内锦标赛要发
// 起的比较次数(同 tournament::AiCost 的同名字段)，evaluation_count 是预
// 选集里要评估的照片张数。
//
// **报一个合并的数、只问一次**（PRD 决策十七、十八）：用户心智里没有"簇
// 内比较"和"簇间挑选"的区别，他说的是"AI 帮我选"，分两次问就得在聊天里
// 解释这个区别。
//
// 返回 false = 不跑：不发起任何视觉调用、不写任何东西，直接返回
// ai_declined=true 的空结果。nullptr(默认) = 无条件继续。
//
// evaluation_count 是**预选集大小**，也就是"最多要评估多少张"。已经评估
// 过的照片会被跳过(缓存判据见 PRD 决策七)，所以实际发出的请求数可能更
// 少 - 闸门只会高报不会低报，方向上是安全的。之所以不能报精确值：闸门必
// 须问在任何一次比较之前，而预选集是从锦标赛的 winner 里裁出来的，那时
// 还不知道 winner 是谁，只知道有几个。
using CurateAiGateFn = std::function<bool(int comparison_count, int evaluation_count)>;

// 票 05：评估阶段的进度，**每张评估发起之前**回调一次。done 是 1-based
// 的"正在评估第几张"，total 是这一轮真正要评估的张数(已缓存的不计入，所
// 以进度一定走得到 total)。
//
// 报在发起之前而不是完成之后，跟 tournament::AiProgressFn 是同一个理由
// (T-8 真机踩出来的)：报"已完成"的话，第一次网络往返结束前屏幕上一个字
// 都不会变，而单张评估就是一次分钟级量级的串行调用。
using EvalProgressFn = std::function<void(int done, int total)>;

// candidate_scope 为空 => 候选范围是整个项目;否则限定到某标签下。
// count > 0。time_window_seconds/hash_threshold 是分簇复用的
// dedup::find_duplicates 参数——调用方(pzt curate 命令)从
// Settings.curate_time_window_seconds/curate_hash_threshold 显式传入
// (curate 本身不读 Settings，跟 dedup::find_duplicates 同一约定)。
// project_id 由调用方保证已存在(headless 命令调用前已经过
// resolve_project_json 校验)。
//
// ai_enabled/ai_provider/local_config(W2026-07-21 目标二新增)：默认
// ai_enabled=false，保证现有调用点零改动。ai_enabled=false 时每簇选
// captured_at 最新的代表，凑够 count 张走现有 farthest-point 多样性；
// ai_enabled=true 时每簇改走单淘汰锦标赛选出 winner，凑够 count 张时从
// winner 集合里随机挑(不做种子化，接受不可复现)。两种模式下候选簇数不
// 足 count 时都返回全部 winner，见 core::tournament::cluster_and_choose
// 的说明。
// on_progress/on_ai_progress（T-8）：原样转给 cluster_and_choose，语义见
// core::dedup::DedupProgressFn 与 core::tournament::AiProgressFn。默认
// nullptr，现有调用点零改动，跟 W2026-07-21 给 dedup 加参数时同一个约
// 定。on_ai_progress 无条件传即可——core 只在 ai_enabled 时才会调它。
//
// on_ai_gate/on_eval_progress/on_cancel（票 05）：curate 从一条纯本地计算
// 的命令变成了会花几分钟、会失败、会超时的命令(它现在自己评估预选集)，
// 于是"开跑前问一句、跑的时候报进度、被拒绝时说清楚"三件事一起补上。语
// 义见上面各自的类型说明与 dedup::CancelFn。默认 nullptr，现有调用点零改
// 动。这一刀同时兑现了上一版这里写死的那条约束——补 on_ai_gate/on_cancel
// 的同时给 CurateResult 补上了 ai_declined/cancelled，否则那句
// `if (summary.clusters.empty())` 会把"用户拒绝"和"候选池本来就是空的"折
// 叠成同一个返回值。见 docs/Intent_Curation_PRD.md 决策十八、十九。
// preselect_multiplier（票 04 的 M）：选片之前先把候选集(每簇一张代表)
// 按时间多样性裁成预选集，规模 = min(ceil(max(1.5, M) · count), 候选集
// 大小)，选择逻辑本身不变、只是面对一个更小的池子。跟
// time_window_seconds/hash_threshold 同一个约定 - curate 不读 Settings，
// 由调用方从 Settings.curate_preselect_multiplier 显式传入。默认 2 与
// Settings 的默认值一致。见 docs/Intent_Curation_PRD.md 决策十至十二：
// 这一刀跑在(将来的)评估之前，因此多样性只沿 captured_at 衡量，也因此评
// 估次数由构造保证有界、与图库大小无关。候选集不足 count 时整条裁剪不参
// 与(那时没有"选"这个动作)。
CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold,
                     double preselect_multiplier = 2.0,
                     bool ai_enabled = false, ai::Provider ai_provider = ai::Provider::Local,
                     const ai::LocalModelConfig& local_config = ai::LocalModelConfig{},
                     dedup::DedupProgressFn on_progress = nullptr,
                     dedup::AiProgressFn on_ai_progress = nullptr,
                     CurateAiGateFn on_ai_gate = nullptr,
                     EvalProgressFn on_eval_progress = nullptr,
                     dedup::CancelFn on_cancel = nullptr);

namespace detail {

// 评估一张图并落库，成功返回 true。抽成可注入的一步是因为票 05 要覆盖的
// 全部行为（闸门报的张数跟真实发起的次数对不对得上、缓存跳过、进度计
// 数、取消）都只存在于**成功路径**上，而 curate 现有的 AI 用例一律是
// "让调用必然失败、只验证退化"，照抄等于零覆盖(PRD 测试决策的现状警
// 告)。跟 tournament::detail::CompareFn、ai::detail::request_*_impl 是同
// 一个先例。
using EvaluateFn = std::function<bool(db::Database&, project::ImageId)>;

// 仅供单元测试使用——evaluate_fn 可注入，不需要真的解码 JPEG 或连网络。
// production 的 curate 就是这个函数塞真实 evaluate_fn 的一层薄封装。
CurateResult curate_impl(db::Database& db, project::ProjectId project_id,
                          std::optional<tagging::TagId> candidate_scope, int count,
                          int time_window_seconds, int hash_threshold, double preselect_multiplier,
                          bool ai_enabled, ai::Provider ai_provider,
                          const ai::LocalModelConfig& local_config, EvaluateFn evaluate_fn,
                          dedup::DedupProgressFn on_progress = nullptr,
                          dedup::AiProgressFn on_ai_progress = nullptr,
                          CurateAiGateFn on_ai_gate = nullptr,
                          EvalProgressFn on_eval_progress = nullptr,
                          dedup::CancelFn on_cancel = nullptr);

// 预选集大小 = min(ceil(max(1.5, multiplier) · count), candidate_count)。
// 暴露出来只为可测：这是票 04 里最需要穷举边界的一块(下界 1.5、上界候选
// 集大小、ceil 的取整方向)，摘成不碰数据库和网络的纯函数才能表驱动覆盖，
// 跟 core/ai 里 detail::request_*_impl 是同一个先例。candidate_count 或
// count 非正时返回 0(curate 的契约保证 count > 0，这里只保证不返回负数)。
int preselect_size(int candidate_count, double multiplier, int count);

}  // namespace detail

}  // namespace pzt::core::curate
