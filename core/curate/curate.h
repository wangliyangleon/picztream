#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/ai/ai.h"
#include "core/ai/selection.h"
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
  // 够那条自票 06 起是**模型返回的顺序**-选与排是同一次决定，返回顺序即
  // 交付顺序(PRD 决策十四)。
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
  // 入)，分开报是因为对用户的含义不同-一个是"没点头"，一个是"点了头又
  // 喊停"。见 core::tournament::ChooseSummary 的同名字段。
  bool cancelled = false;
  // 票 06（PRD 决策二十一）：模型的选择整批没用上-调用失败，或者返回值
  // 清洗完剩下的有效序号不足 count-这一次的选择与排序退化成了确定性路
  // 径。ai_enabled=false 时恒为 false（那条路本来就是确定性的，谈不上退
  // 化）。
  //
  // **刻意不复用 ai_fallback_count**：那个数的语义是"某**几个簇**的比较失
  // 败、那几簇退化成选 captured_at 最新的一张"，而且已经进了用户话术("哪
  // 几组不是 AI 挑的")。整批的选择退化是另一回事-照片是 AI 挑的还是按时
  // 间挑的，说错了用户会当场发现。两个信号互不干扰：一次运行里可以只有其
  // 中一个为真，也可以两个同时为真。
  bool ai_selection_fallback = false;
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
// evaluation_count 有两种精度，取决于这一趟有没有簇要跑锦标赛：
//
// - 有比较要跑时闸门必须问在第一次比较之前，那时预选集还没裁出来(它是从
//   winner 里裁的，而 winner 由锦标赛决定)，只知道候选有几个。此时报的是
//   **预选集大小**，即"最多要评估多少张"；命中缓存的照片会被跳过(判据见
//   PRD 决策七)，所以实际请求数可能更少。
// - 一次比较都不用跑时(全是单例簇)，预选集此刻已经确定，报的是**扣掉缓
//   存之后的精确张数**。
//
// 两种情况都只会高报不会低报，方向上一致。
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
// ai_enabled=true 时每簇改走单淘汰锦标赛选出 winner，凑够 count 张时由
// 模型读着预选集里每张的质量评价与内容描述连选带排(票 06；此前是
// std::sample 的随机抽样，因为那时没有任何可比的东西)。模型没给出能用
// 的答案时整批退化成上面那条确定性路径，并置 ai_selection_fallback。两
// 种模式下候选簇数不足 count 时都返回全部 winner，见
// core::tournament::cluster_and_choose 的说明。
// on_progress/on_ai_progress（T-8）：原样转给 cluster_and_choose，语义见
// core::dedup::DedupProgressFn 与 core::tournament::AiProgressFn。默认
// nullptr，现有调用点零改动，跟 W2026-07-21 给 dedup 加参数时同一个约
// 定。on_ai_progress 无条件传即可——core 只在 ai_enabled 时才会调它。
//
// on_ai_gate/on_eval_progress/on_cancel（票 05）：curate 从一条纯本地计算
// 的命令变成了会花几分钟、会失败、会超时的命令(它现在自己评估预选集)，
// 于是"开跑前问一句、跑的时候报进度、被拒绝时说清楚"三件事一起补上。语
// 义见上面各自的类型说明与 dedup::CancelFn。默认 nullptr，现有调用点零改
// 动。这一刀同时兑现了上一版这里写死的那条约束-补 on_ai_gate/on_cancel
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
// selection_brief（票 08）：用户这次想要什么 - 用途、题材偏好、叙事结构提
// 炼成的一段自由文本，原样进模型的选择提示词（语义与切分理由见
// core::ai::request_selection 与 PRD 决策二）。**是一段提炼过的话，不是意
// 图原文**：原文里混着去重、服务商、寒暄这些跟选哪几张无关的东西，透传等
// 于让提示词被噪声污染，提炼是 agent 的活。为空(默认)时那一段整个不进提示
// 词，与票 08 之前的行为逐字相同。ai_enabled=false 时无处可用，忽略。
CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold,
                     double preselect_multiplier = 2.0,
                     bool ai_enabled = false, ai::Provider ai_provider = ai::Provider::Local,
                     const ai::LocalModelConfig& local_config = ai::LocalModelConfig{},
                     const std::string& selection_brief = "",
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

// 票 06：让模型读着预选集的描述连选带排的那一步，同样抽成可注入的一步。
// 参数是按预选集顺序排好的候选描述与要选的张数，返回模型给的 1-based 序
// 号；调用失败(网络/解析)返回 nullopt。返回值不做清洗，那是
// resolve_selection 的事。
//
// production 的 curate 塞的是 ai::request_selection 的薄封装。传 nullptr
// (测试里的默认)等价于"模型不可用"，走整批退化。
using SelectFn = std::function<std::optional<std::vector<int>>(
    const std::vector<ai::SelectionCandidate>& candidates, int count)>;

// 仅供单元测试使用-evaluate_fn/select_fn 可注入，不需要真的解码 JPEG 或连
// 网络。production 的 curate 就是这个函数塞真实实现的一层薄封装。
CurateResult curate_impl(db::Database& db, project::ProjectId project_id,
                          std::optional<tagging::TagId> candidate_scope, int count,
                          int time_window_seconds, int hash_threshold, double preselect_multiplier,
                          bool ai_enabled, ai::Provider ai_provider,
                          const ai::LocalModelConfig& local_config, EvaluateFn evaluate_fn,
                          SelectFn select_fn = nullptr,
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

// 票 06（PRD 决策十三）：模型返回的原始序号 -> 最终选择。
//
// 1. 剔除不在 1..pool_size 的序号、去重，**保序**
// 2. 剩余 >= count：取前 count 返回（1-based，调用方按它翻译回照片）
// 3. 剩余 < count：返回**空**，表示这一批整体退化成确定性选择
//
// 不选"任何不合法就整体退化"：模型返回 9 个好序号外加 1 个越界的，把整批
// 扔掉丢的是真信号。也不选"不足时用确定性结果补齐"：补进来的照片既不符合
// 用户的题材偏好、也不在模型排的叙事顺序里，交付的会是两套逻辑拼接的结
// 果，而用户看到的话术只有一种。
//
// 跟 preselect_size 一样摘成纯函数只为可测：这是本票最容易写错、最需要穷
// 举边界的一块（越界、重复、恰好卡在 count 上下），不碰网络与数据库才能表
// 驱动覆盖。count 非正时返回空（curate 的契约保证 count > 0）。
std::vector<int> resolve_selection(const std::vector<int>& raw, int pool_size, int count);

}  // namespace detail

}  // namespace pzt::core::curate
