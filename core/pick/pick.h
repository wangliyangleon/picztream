#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/dedup/dedup.h"
#include "core/project/project.h"
#include "core/result.h"
#include "core/tournament/tournament.h"

// 两级人工选片：从一批候选里选出 N 张留下，其余判废。
//
// 第一级在簇内决出簇冠军，第二级在簇冠军之间取 top-N。两级都用同一套
// bracket 推进(core/tournament)，区别只在"一次比较由谁做出来" - 这一层
// 拿到的是一个中立的比较原语，等的是一次按键还是一次网络往返它不知道。
//
// 为什么分两级而不是在全范围上直接跑一棵树：比较次数是同一量级，但每一
// 次比较的性质完全不同。两级结构下绝大多数比较落在同一场景内("这个连拍
// 里哪张眼睛睁着")，是人判断最快、最有把握的那类；扁平结构下大部分比较
// 是"海景 vs 人像"，既慢又判不准。
//
// 与 core/curate 的分工：curate 是 AI 按意图跨簇挑代表作，pick 是人两两
// 比出来的；两者共用 dedup 的分簇与 curate 那组粗参数，选择逻辑不共用。
namespace pzt::core::pick {

// 一次比较：给两张已解码的图，说赢的是左边还是右边。就是锦标赛那一套原
// 语，这里引进本命名空间，不是另一个类型。
using ComparisonWinner = tournament::detail::ComparisonWinner;
using CompareFn = tournament::detail::CompareFn;

// 中途取消(语义见 dedup.h 的 CancelFn，必须粘性)。查到 true 就返回
// cancelled=true 的空结果，一个标签都不写 - 写库统一在最后一步，所以这
// 里不需要回滚。
using CancelFn = dedup::CancelFn;

enum class PickError {
  // 项目不存在。
  ProjectNotFound,
  // 最后那一步批量落库整批失败，因而**一张都没打上**(批量接口是全有全
  // 无的)。走错误通道而不是回一个 Ok 加一个计数为 0 的结果：选出来了却
  // 一张都没记下来，跟选片成功不是同一件事，而调用方要拿这个结果对用户
  // 说"Y 张打上废片"。
  RejectTagWriteFailed,
};

// 开跑前报得出的开销。分簇已经跑完(本地、便宜、无副作用)，但一次比较都
// 还没发起、一个标签都还没写，所以这是唯一一个"能报出真实开销、且拒绝之
// 后系统状态跟没执行过完全一样"的位置。
struct PickCost {
  // 排除废片/重复之后的候选总数 C。
  int candidate_count = 0;
  // 簇冠军池 m = 多图簇数 + 单例数。单例不经任何淘汰直接进第二级，所以
  // 它跟一个 8 连拍里杀出来的冠军在决赛里平起平坐 - 这是"先分簇再选"这
  // 个结构固有的。
  int champion_count = 0;
  // 第一级的比较次数 C - m，**精确值**：单淘汰 k 个成员恰好 k-1 场。
  int first_stage_comparisons = 0;
  // 两级合计的比较次数**上界**。第二级的实际次数会因为轮空少几场，所以
  // 这个数只能是上界；调用方的文案要说"最多需要 X 次"，报一个用户实际到
  // 不了的数字、让进度条走到一半就结束，比一开始就说"最多"更糟。第一级
  // 那一段仍然是精确的。
  int max_comparisons = 0;
  // 跑完之后会被打上废片标签的张数 Y = C - min(N, m)。
  int reject_count = 0;
};

// 真正开跑前的闸门，在分簇跑完、任何一次比较发起之前调用一次。返回
// false = 不跑：不发起任何比较、不写任何标签，直接返回 declined=true 的
// 空结果。nullptr(默认) = 无条件继续。
//
// 候选不足那条短路上不会被调到 - 那时没有任何要确认的开销(恒为零比较零
// 写入)，问了也没有意义。
using PickGateFn = std::function<bool(const PickCost&)>;

enum class PickStage {
  Cluster,  // 第一级：某个簇内决簇冠军
  Final,    // 第二级：簇冠军之间取名次
};

// **每一场比较发起之前**回调一次(不是之后)。报"已完成"的话，第一次比较
// 的画面画出来之前屏幕上一个字都不会变。
struct PickProgress {
  PickStage stage = PickStage::Cluster;
  // Cluster 阶段：正在跑第几组 / 共几组，1-based。**只数真的要跑比较的
  // 组**(成员数 >= 2)，单例不发起比较也就不占一格，所以 group_total 通
  // 常小于 PickCost::champion_count - 这样进度才走得到分母。跟
  // tournament::AiProgress 的 group_* 数的是同一类东西。
  int group_index = 0;
  int group_total = 0;
  // Cluster 阶段：本组的第几场 / 共几场(= 组员数 - 1)，1-based。
  int match_index = 0;
  int match_total = 0;
  // Final 阶段：正在选第几张 / 一共要选几张(= min(N, m))，1-based。
  int rank_index = 0;
  int rank_total = 0;
  // 两级共用：含这一场在内已经发起了第几次比较，以及 PickCost 报出的那
  // 个上界。分母跟闸门报的必须是同一个数，否则用户点头时看到的开销和进
  // 度条走的刻度对不上。
  int comparisons_done = 0;
  int max_comparisons = 0;
};

using PickProgressFn = std::function<void(const PickProgress&)>;

struct PickResult {
  // 留下的 min(N, m) 张。跑了第二级时按名次排列；N >= m 跳过第二级时按
  // 簇的顺序(多图簇在前、单例按 id 升序)，那条路上没有名次可言。
  std::vector<project::ImageId> selected;
  // 开跑前算出来的那批数，原样带回来：候选不足那条短路不会调闸门，而调
  // 用方仍然要拿 C 说话("候选 C 张不足 N 张")。
  PickCost cost;
  // 真正发起了的比较次数，<= cost.max_comparisons。
  int comparisons_done = 0;
  // 实际打上废片标签的张数。正常跑完时等于 cost.reject_count；上一次已
  // 经带着废片标签的图片不会出现在候选里，所以这两个数只会在下面三个
  // flag 为真时(零写入)分开。
  int rejected_count = 0;
  // C <= N(含 C == 0)：一次比较都没发起、一个标签都没打。selected 为
  // 空 - **不是**"这 C 张都留下了"，调用方该报"候选不足"而不是把空结果
  // 当成选择结果。count 非正(调用方违约)也落在这条短路上：不放大一个契
  // 约错误，按同样的零比较零写入返回。
  bool insufficient_candidates = false;
  // 闸门返回了 false(调用方看过真实开销之后不跑了)。selected 为空、零写
  // 入。
  bool declined = false;
  // CancelFn 返回了 true，或者比较原语中途放弃(人在环这条路上两者是同一
  // 件事：没有"AI 失败"这种第三种可能)。同样是 selected 为空、零写入。
  // 跟 declined 分开报是因为对用户的含义不同 - 一个是"没点头"，一个是
  // "点了头又喊停"，跟 tournament::ChooseSummary 同名字段一致。
  bool cancelled = false;
};

// image_ids 是调用方已经解析好的范围。
//
// 候选先排除废片与重复标签，**排除发生在分簇之前**：一个成员全是废片的
// 簇根本不会成形，于是不存在"这一簇没有冠军"这个需要特殊处理的状态；一
// 个 5 连拍里已被手动打过 3 张废片的，就按剩下 2 张比一场。这是本模块依
// 赖的不变量，不能把顺序倒过来。它同时让重跑收敛：上一次判废的这一次不
// 是候选，所以"选 20 张"之后再"选 10 张"是在那 20 张里选，不是重新洗一
// 遍全库。
//
// time_window_seconds/hash_threshold 是分簇参数，跟 curate 一样由调用方
// 显式传入(core 不读 Settings)。要传的是 curate 那组**粗参数**：pick 要
// 的是"同一场景"的粒度而不是"近乎同一张"的粒度。
//
// count 是要留下的张数 N，必须为正。N >= m 时跳过第二级，m 个簇冠军全部
// 入选，**不从各簇亚军补位凑够 N** - pick 的语义是每个场景只留最好的一
// 张，连拍里的第二好本来就是该被淘汰的那张。
//
// 未入选的全部打上废片标签，入选的**不打任何标签**："被选中"的表示就是
// "没有废片标签"，不引入第四个系统标签。落库是一个事务包住全部写入，且
// 只发生在最后一步，所以取消路径不需要任何回滚。
Result<PickResult, PickError> pick(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int count, int time_window_seconds, int hash_threshold, CompareFn compare_fn,
    PickGateFn on_gate = nullptr, PickProgressFn on_progress = nullptr, CancelFn on_cancel = nullptr);

namespace detail {

// 仅供单元测试使用 - decode_fn 也可注入，不需要真的解码 JPEG 就能验证分
// 簇后处理、两级推进、短路与落库。跟 tournament::detail::
// cluster_and_choose_impl、dedup::detail::find_duplicates_impl 是同一个
// 模式：上面的 pick 就是这个函数塞真实 decode_fn 的一层薄封装。
//
// compare_fn 在两级里都不带默认实现 - 它就是"人按了哪一边"，production
// 与测试各自注入，这一层没有可以兜底的默认答案。
Result<PickResult, PickError> pick_impl(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int count, int time_window_seconds, int hash_threshold,
    dedup::detail::PreviewDecodeFn decode_fn, CompareFn compare_fn, PickGateFn on_gate = nullptr,
    PickProgressFn on_progress = nullptr, CancelFn on_cancel = nullptr);

}  // namespace detail

}  // namespace pzt::core::pick
