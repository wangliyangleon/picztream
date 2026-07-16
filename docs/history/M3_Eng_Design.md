# PicZTream (PZT) Milestone 3 工程设计文档（增量一：选片辅助评分——主动触发）

## 背景

`docs/M3_PRD.md` 定的产品行为：`pzt open` 里的 `:` 命令行触发一次异步 AI 请求，用户输入的内容是可选的"额外指引"，不是完整 prompt。这份工程设计文档原来是围绕"审美评分"（笼统 1-100 分 + 点评）写的，主动触发链路本身（发起请求→异步执行→落库→浏览时看到）已经实现、真机验证过，不受这次改动影响。这次是文档的整体重写，不是小范围打补丁——PRD 那边把 AI 的评估内容从"审美"改成了"曝光/构图/对焦"三个独立技术维度，输出形状从"一个分数+一段点评"变成"三组分数+原因(+修正建议)+一句总体归纳"，数据量和结构复杂度都上了一个台阶，值得重新过一遍每一层的设计。

沿用上一版就定下的分层原则：**"发起一次带图片的 AI 请求"这一层继续为未来复用而设计，不知道"exposure/composition/focus"是什么**，只负责"发起请求、按调用方描述的形状要求 JSON、把解析出来的 JSON 原样交还"。这一层（`core/ai/ai.h`/`.cpp`）这次完全不用动。上面消费这一层的具体业务逻辑——现在改名叫 `core::ai::evaluation`（原来叫 `score`，见下面"命名"一节的说明）——知道要从结果里取哪些字段、怎么校验、综合分数和达标状态怎么算、存进哪张表。

## 现有代码基础（延续上一版，未变）

* **`core/browse/prefetch.h`/`.cpp` 的 `PrefetchCache`**：单 `jthread`（成员声明在最后，保证先于其它成员析构）+ `mutex` + `condition_variable_any` 的异步骨架，`EvaluationWorker`（原 `ScoreWorker`）继续照抄这个骨架，接口保持非阻塞轮询。
* **`core/db/database.h`**：`ScoreWorker`/`EvaluationWorker` 的后台线程各自开 `db::Database::open_at(db_path_)` 连接（`db_path_` 可注入，默认 `db::default_db_path()`，见 `core/ai/score_worker.h` 已经落地的这个约定），不跨线程共享主线程连接。
* **`cli/ui/ui.cpp` 的 `stdin_ready()` + `cli/commands/browse.cpp` 的 poll 逻辑**：AI 请求挂起时也要 poll，但只有请求真的完成时才重绘（`consume_new_result` 那套 generation 计数器），这套机制已经实现好，这次不用改。
* **`core/db/schema.cpp` 的 `ensure_column`/`column_exists`**：这次新增列/新表继续用得上，但这次还要用到**删除列**（`ALTER TABLE ... DROP COLUMN`，SQLite 3.35+ 原生支持，项目依赖的版本已经够新），这是第一次在这个项目里删列，见"数据库 Schema 设计"一节。
* **`core/recipe/recipe.h` 的"拒绝 JSON blob、坚持 typed column"先例**：这次继续遵循，但"要不要挤在同一张表"是另一个问题，这次改成开一张新表（见下）——先例约束的是"别用 JSON 字符串偷懒"，没有约束"必须挤在 `images` 表上"。

## 数据库 Schema 设计

### 为什么这次开一张新表，不再继续加列到 `images` 上

上一版是 4 列（`ai_score`/`ai_score_comment`/`ai_score_prompt`/`ai_score_provider`），这次三个维度各自的分数+原因就是 6 列，加曝光的修正百分比、构图的旋转角度+四个方向裁切百分比，再加总体点评、额外指引原文、供应商，是十四列。继续堆在 `images` 表上会让这张表的职责从"文件本身的元数据"混进"AI 评估结果"，这次改成一张新表 `image_evaluations`，`images` 表保持不变（上一版加的 4 列全部删除，见下）。

### 删除旧列，清空旧数据

```sql
ALTER TABLE images DROP COLUMN ai_score;
ALTER TABLE images DROP COLUMN ai_score_comment;
ALTER TABLE images DROP COLUMN ai_score_prompt;
ALTER TABLE images DROP COLUMN ai_score_provider;
```

库里现在这几列的数据都是这次迭代过程里测出来的测试数据，没有需要保留的真实用户数据，不写迁移/兼容逻辑——直接删列，`ensure_column`/`column_exists` 那套幂等迁移只管"确保有"，删列不需要幂等（`initialize_schema` 每次跑都执行同一条 `DROP COLUMN`，列已经不存在时 SQLite 会报错，需要一个 `column_exists` 判断包一下，跟 `ensure_column` 反过来的写法，同样幂等）。

### 新表

```sql
CREATE TABLE IF NOT EXISTS image_evaluations (
  image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
  exposure_score INTEGER NOT NULL,
  exposure_note TEXT NOT NULL,
  exposure_fix_percent REAL,          -- 可空:分数已经够高、模型判断不需要修正建议时不给
  composition_score INTEGER NOT NULL,
  composition_note TEXT NOT NULL,
  composition_fix_rotate_degrees REAL,        -- 构图的修正建议四个字段同生共死:
  composition_fix_crop_left_percent REAL,     -- 要么都有值要么都是 NULL,不会出现只填了
  composition_fix_crop_right_percent REAL,    -- 旋转角度、裁切值缺失的情况——这四列合起来
  composition_fix_crop_top_percent REAL,      -- 表示"一个 CompositionFix"这一个概念，不是
  composition_fix_crop_bottom_percent REAL,   -- 四个互相独立的可选值。
  focus_score INTEGER NOT NULL,
  focus_note TEXT NOT NULL,
  comment TEXT NOT NULL,              -- 跨三项的一句总体归纳,不是某一项的 note
  extra_guidance TEXT NOT NULL,       -- 触发这次评估时用户输入的额外指引原文(可能是空字符串)
  provider TEXT NOT NULL              -- "claude" | "gemini"
);
```

`image_id` 直接当主键（一对一关系，不单独设自增 id），`ON DELETE CASCADE` 跟 `tags`/`image_tags` 现有的级联删除惯例一致——项目删除时这张表的行跟着自动清掉，不需要额外代码。

这张表整行要么存在（评估过，所有字段都有值）要么不存在（没评估过/评估失败，`image_id` 在表里没有对应行）——不是 `images` 表上那种"整行都在、单个字段可空"的语义，所以除了本来就允许 `NULL` 的四个修正建议字段之外，其它字段都是 `NOT NULL`：评估要么完整成功要么整行都不写，不会出现"有分数没原因"这种半成品行。评估失败时 `EvaluationWorker` 不 `INSERT`/`UPDATE` 这张表，也不删除已有的旧评估结果（旧结果仍然是有效信息，一次失败的重新评估不该把之前成功的结果抹掉）。

**综合分数（三项平均，四舍五入）和是否达标（三项都 ≥ 6）不入库**——这两个值完全由 `exposure_score`/`composition_score`/`focus_score` 三列算出来，存成列反而多一条"存出来的值可能跟三项分数对不上"的风险（比如以后调整阈值，历史行的达标状态还留着按旧阈值算出来的值）。这次的数据量级（单项目几百到几千张图）用不上"预先算好、直接 `ORDER BY`"这种优化，现算的开销可以忽略。

### `core::project::ImageInfo` 扩展

```cpp
// core/project/project.h
#include "core/ai/evaluation.h"  // EvaluationInfo 定义在这里,project.h 依赖 ai.h 的纯数据类型

struct ImageInfo {
  // ...现有字段不变(id/project_id/file_path/file_name/file_size/kind/
  // preview_cache_path/captured_at)，上一版的 ai_score 等四个字段删除...
  std::optional<ai::EvaluationInfo> evaluation;
};
```

`get_image()` 的 SQL 改成 `LEFT JOIN image_evaluations ON images.id = image_evaluations.image_id`，`image_evaluations` 那部分的列全是 `NULL`（没有匹配行）时 `evaluation` 留 `nullopt`，否则整个 `EvaluationInfo` 一次性填好——不会出现"部分字段有值部分没有"的中间状态，因为上面已经说过这张表要么整行都在要么整行不在。

`core::ai::evaluation.h` 里定义（细节见下一节）：

```cpp
namespace pzt::core::ai {

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

// 落库/读回用的完整形状——比 request_evaluation() 直接返回的 EvaluationResult
// (纯模型输出:三个维度+comment)多两个字段(extra_guidance/provider)，这两个
// 不是模型返回的,是发起请求时已经知道的上下文，由 EvaluationWorker 写库时一
// 起塞进去。
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

// 综合分数/达标判断不入库，现算——见"数据库 Schema 设计"一节的说明。这两个
// 函数是这份契约的唯一实现,CLI 展示和以后近似重复检测排序都调这两个函数,
// 不会出现两处各自算一遍、算法不一致的风险。
int overall_score(const EvaluationInfo& info);   // round((exposure.score + composition.score + focus.score) / 3.0)
bool passes_gate(const EvaluationInfo& info);     // exposure.score >= 6 && composition.score >= 6 && focus.score >= 6

}  // namespace pzt::core::ai
```

`kGateThreshold = 6` 是一个 `core::ai` 内部常量，不暴露成可配置项——这次没有谁要求它可调，不提前设计配置层。

## core/api 接口设计

### `core/ai/ai.h`/`.cpp`：通用层，不变

`Provider`/`RequestError`/`HttpResponse`/`HttpPostFn`/`perform_curl_post`/`request_json`/`to_string(Provider)` 全部原样保留，这次完全不touch这个文件——`request_json` 本来就不知道"score"是什么，现在也不需要知道"exposure/composition/focus"是什么，这正是上一版设计这一层时要达到的效果。

### `core/ai/evaluation.h`/`.cpp`：选片评估——`request_json` 的消费者，取代 `score.h`

`score.h`/`score.cpp`/`score_test.cpp` 整个删除，换成 `evaluation.h`/`evaluation.cpp`/`evaluation_test.cpp`。

```cpp
namespace pzt::core::ai {

enum class EvaluationError { MissingApiKey, NetworkError, HttpError, ParseError, OutOfRange };

struct EvaluationResult {  // 纯模型输出,不含 extra_guidance/provider(见 EvaluationInfo 的说明)
  DimensionAssessment exposure;
  std::optional<ExposureFix> exposure_fix;
  DimensionAssessment composition;
  std::optional<CompositionFix> composition_fix;
  DimensionAssessment focus;
  std::string comment;
};

// extra_guidance:用户在 `:` 里输入的原始文本，可能是空字符串。内部拼出完整
// 的评估任务描述——固定模板(要求模型从曝光、构图、对焦三个技术维度分别评估，
// 不涉及色彩、情绪表达这类风格判断)后面跟一段"Additional guidance:
// {extra_guidance}"(为空时省略)，作为 user_prompt 传给 request_json；
// schema_instruction 描述上面 EvaluationResult 对应的 JSON 形状(三个维度对象
// +可选的 fix 字段+comment)。从结果 JSON 里取值、校验三个 score 字段都落在
// 0-10——任何一个取不到、类型不对、或者越界，整体算失败，不写库
// (EvaluationError::OutOfRange)。RequestError 直接映射到同名的
// EvaluationError。模板本身不会展示给用户看，固定用英文，不跟着 cli::i18n
// 走(用户的额外指引本身可以是任何语言)。
Result<EvaluationResult, EvaluationError> request_evaluation(const decode::DecodedImage& image,
                                                               const std::string& extra_guidance,
                                                               Provider provider);

namespace detail {
// 跟上一版 score.h 的 detail::request_score_impl 同样的理由:公开的
// request_evaluation 签名要精确匹配 EvaluationWorker::EvaluationFn(3 个参
// 数)，可以直接当默认值用；这个版本多一个可注入的 HttpPostFn，仅供单元测试
// 验证 prompt 拼接/字段提取/越界校验，不需要真的连网络。
Result<EvaluationResult, EvaluationError> request_evaluation_impl(const decode::DecodedImage& image,
                                                                    const std::string& extra_guidance,
                                                                    Provider provider,
                                                                    HttpPostFn http_post);
}  // namespace detail

}  // namespace pzt::core::ai
```

**JSON 形状**（模型被要求返回的样子，示例）：
```json
{
  "exposure": {"score": 7, "note": "轻微欠曝", "fix_percent": 15},
  "composition": {"score": 4, "note": "地平线明显倾斜",
                  "fix": {"rotate_degrees": 2.5, "crop_left_percent": 0, "crop_right_percent": 0,
                          "crop_top_percent": 0, "crop_bottom_percent": 5}},
  "focus": {"score": 9, "note": "清晰"},
  "comment": "整体不错，主要问题是构图水平线没找平。"
}
```
`exposure.fix_percent`/`composition.fix` 允许模型直接不给这个字段(分数已经够高、判断不需要修正建议时)，解析时按"缺失=没有修正建议"处理，不强迫模型硬凑一个"建议调整 0%"。`focus` 没有对应的 fix 字段，这次的 schema_instruction 也不会问模型要——对焦软了没有后期能修的办法。

以后如果要加自动打标签这类新能力，还是同一个模式：新文件、新 schema_instruction、新的结果结构体，调用同一个 `ai::request_json`，`ai.h`/`ai.cpp` 不用动。

### `core/ai/evaluation_worker.h`/`.cpp`：异步+去重+落库，取代 `score_worker.h`

骨架跟上一版 `ScoreWorker`完全一样(单 `jthread`+`mutex`+`condition_variable_any`+FIFO 队列+去重 `in_flight_` 集合+`generation_` 计数器+可注入的 db 路径)，改动只有两处：

1. 类名/文件名 `ScoreWorker`→`EvaluationWorker`，`ScoreFn`→`EvaluationFn`(签名对应换成 `EvaluationResult`/`EvaluationError`)，默认值指向新的 `request_evaluation`。
2. 请求处理成功之后的落库语句，从"`UPDATE images SET ai_score=... WHERE id=?`"换成"`INSERT INTO image_evaluations (...) VALUES (...) ON CONFLICT(image_id) DO UPDATE SET ...`"(`image_id` 是主键，重复评估同一张图要么是第一次插入、要么是覆盖旧评估，用 `ON CONFLICT` 一条语句处理两种情况，不用先查再决定 INSERT 还是 UPDATE)。

```cpp
namespace pzt::core::ai {

class EvaluationWorker {
 public:
  using EvaluationFn = std::function<Result<EvaluationResult, EvaluationError>(
      const decode::DecodedImage&, const std::string&, Provider)>;

  explicit EvaluationWorker(std::string db_path = db::default_db_path(),
                             EvaluationFn evaluation_fn = request_evaluation);
  ~EvaluationWorker();

  EvaluationWorker(const EvaluationWorker&) = delete;
  EvaluationWorker& operator=(const EvaluationWorker&) = delete;

  bool request(project::ImageId image_id, Provider provider, const std::string& extra_guidance);
  bool has_pending() const;
  bool consume_new_result(std::uint64_t& last_seen_generation) const;

  // `/tasks` 命令用——只要聚合数字（排队几个、有没有正在处理），不暴露
  // 具体是哪几张图片（PRD 明确不需要看到明细）。实现很直接：
  // in_flight_ 记录的是"排队中 + 正在处理"的并集，queue_ 只记录"排队
  // 中、还没被 worker 线程取走"的那部分——由于 worker 只有一个线程，同一
  // 时刻最多有一个请求处于"已经从 queue_ 弹出、但还没处理完"的状态，所
  // 以 in_flight_.size() - queue_.size() 只会是 0 或 1，直接当
  // processing 用，不需要额外的状态变量。
  struct QueueStatus {
    std::size_t queued;
    bool processing;
  };
  QueueStatus queue_status() const;

 private:
  // ...跟 ScoreWorker 一样的私有成员/worker_loop/process_request 骨架...
};

}  // namespace pzt::core::ai
```

`process_request` 里失败路径的行为：解码失败/AI 请求失败都只打 debug 日志、不写 `image_evaluations` 表，**不删除已有的旧评估结果**——这一点跟上一版"四列都不写、保持 NULL"的效果类似，但这次因为是独立的一行，逻辑上更清楚：失败就是这次请求没有产出，不代表要清空历史。

### 批量提交：不需要改 `EvaluationWorker`，CLI 侧循环调 `request()`

`EvaluationWorker` 的队列本来就支持同时排着多个不同 `image_id` 的请求（FIFO 处理），`/ai_eval` 命令不需要给 `EvaluationWorker` 加"批量提交"的接口——CLI 侧拿到这一批要处理的 `image_id` 列表之后，对每一个还没评估过的依次调用现有的 `request()` 就够。`request()` 本身的去重逻辑（`in_flight_` 检查）天然保证了批量提交跟单张手动触发不会互相冲突。

## CLI 接线

### `cli/ui`：不变

`read_text_line_with_placeholder`（占位提示、显示/定位光标、超宽换行）这一整套已经实现且验证过，跟评估的具体内容无关，这次不用碰。

### `cli/commands/browse.cpp`：`ScoreWorker`→`EvaluationWorker` 改名，信息栏展示重做

* 构造点、`:` 键 dispatch、poll 逻辑里的 `ScoreWorker`/`score_worker`/`Provider::Gemini` 调用点，机械地换成 `EvaluationWorker`/`evaluation_worker`——行为不变（生命周期、去重、poll-only-on-real-result 这些逻辑本身这次不用改）。
* 信息栏（metadata block）原来的两行（"AI Score:"/点评）换成信息量大得多的一段，紧跟"风格"区块之后、复用 `emit_line` 的越界裁剪：

  ```
  Culling: 7/10 (E) 4/10 (C) 9/10 (F)  [FAIL]
    Exposure: 轻微欠曝 (建议 +15%)
    Composition: 地平线明显倾斜 (建议旋转 2.5°)
    Focus: 清晰
  整体不错，主要问题是构图水平线没找平。
  ```
  具体行怎么排、要不要缩写"曝光/构图/对焦"、`[FAIL]`/`[PASS]` 用什么样式标记，这些是实现时才定的展示细节，这份文档只锁定信息本身要包含：三项各自的分数、三项各自的 note、达标与否、曝光/构图的修正建议（有的话）、总体 comment。`overall_score()`/`passes_gate()` 直接调 `core::ai` 那两个函数，不在 CLI 层重新实现一遍算法。
  这一块比原来两行明显更高，信息栏原来就因为这次改动之前的"metadata block 拉高"而留出了空间（`docs/M3_Eng_Design.md` 上一版实现阶段真机反馈过的 flicker/two-scores 那些坑，改动 metadata 展示内容时要留意同一批教训：分块渲染要批到一次 `write_stdout`、要覆盖到没内容的行，不能只清"最后一行之后"）。

### `cli/commands/browse.cpp`：`/` 命令解析

`/ai_eval`/`/tasks` 现在真正实现了，跟这一节最初的设计相比有几处实质性出入（都是这一轮真实需求驱动的调整，不是简单的代码搬运）：

* **控制台现在要求所有输入必须以 `/` 开头**——原设计里"裸文本直接当额外指引提交"、"直接回车照样提交一次评分请求"这两条路径整个删掉了。真实使用中发现这是一个误触发风险：手快按了回车、或者忘了打 `/` 就开始打字，会在不知情的情况下对当前图片发起一次评估请求。现在这两种输入统一走 `msg_console_requires_slash()`，不提交任何东西，Esc 才是真正的取消。
* **对当前图片评估现在也要走 `/ai_eval`**——不再有独立的"裸文本 = 当前图片额外指引"路径，而是 `/ai_eval` 这一条命令名根据后面有没有范围标记（`*` 或 `#标签名`）分岔成"批量"和"当前图片"两种行为。这是为了让整个控制台的"如何区分命令 vs 内容"这件事只有一条规则（必须显式标记），不是"批量命令需要 `/` 前缀，当前图片评估不需要"这种不对称的两套规则。
* **标签范围语法从裸标签名改成 `#标签名`**——`/ai_eval <标签名>` 变成 `/ai_eval #标签名`。原因是 `/ai_eval` 单条命令名要同时支持"批量"和"当前图片额外指引"两种用法，如果范围标记是裸标签名，没法区分"`/ai_eval sunset`"到底是"批量评估叫 sunset 的标签"还是"对当前图片的额外指引就是"sunset""这两种意图。`#` 前缀消掉这个歧义。`/dedup` 的标签范围语法也同步改成 `#标签名`，两个命令共用同一套"范围怎么写"的规则和同一份解析代码（`resolve_console_scope`），不是各自维护一套。
* `handle_ai_console_command` 因此比原设计多带一个 `pzt::core::ImageId current_image_id` 参数（当前图片模式要知道请求提交给哪张图）；`msg_ai_unknown_command()` 也改成带参数的 `msg_ai_unknown_command(const std::string& command)`，把具体命令名回显进提示，比不带名字的版本更好定位问题。

`handle_ai_prompt_flow`（`:` 键主入口）现在是：

```cpp
std::string handle_ai_prompt_flow(pzt::core::EvaluationWorker& evaluation_worker,
                                   pzt::core::ProjectId project_id, pzt::core::ImageId image_id,
                                   int banner_row, int start_col, int content_cols) {
  auto input = read_text_line_with_placeholder(pzt::cli::i18n::msg_ai_prompt_placeholder(),
                                                 banner_row, start_col, content_cols);
  if (!input) return "";  // Esc,静默取消
  if (input->empty() || (*input)[0] != '/') {
    return pzt::cli::i18n::msg_console_requires_slash();
  }
  return handle_ai_console_command(evaluation_worker, project_id, image_id, *input, banner_row,
                                    start_col, content_cols);
}
```

`resolve_console_scope`：`/dedup`/`/ai_eval` 共用的批量范围解析，`*` 走 `list_images`，`#标签名` 走 `find_tag_by_name`+`filter_by_tag`（标签不存在或 scope 既不是 `*` 也不以 `#` 开头都返回一条错误文案，不静默当成空范围或裸标签名处理）：

```cpp
struct ScopeResolution {
  std::vector<pzt::core::ImageId> image_ids;
  std::string error_message;  // 非空表示解析失败，caller 直接把它当结果返回
};

ScopeResolution resolve_console_scope(pzt::core::ProjectId project_id, const std::string& scope);
```

`handle_ai_console_command`：`ai_eval` 分支里，先看 `rest` 的第一个 token 是不是范围标记，是的话走批量（`handle_ai_eval_command`），不是的话整段 `rest` 就是当前图片的额外指引，直接提交：

```cpp
std::string handle_ai_console_command(pzt::core::EvaluationWorker& evaluation_worker,
                                       pzt::core::ProjectId project_id,
                                       pzt::core::ImageId current_image_id, const std::string& input,
                                       int banner_row, int start_col, int content_cols) {
  auto [command, rest] = split_console_command(input);
  if (command == "dedup") return handle_dedup_command(project_id, rest, banner_row, start_col, content_cols);
  if (command == "tasks") return handle_tasks_command(evaluation_worker);
  if (command == "ai_eval") {
    std::istringstream iss(rest);
    std::string first_token;
    iss >> first_token;
    bool is_batch_scope = first_token == "*" || (!first_token.empty() && first_token[0] == '#');
    if (is_batch_scope) {
      std::string extra_guidance;
      std::getline(iss, extra_guidance);
      std::size_t start = extra_guidance.find_first_not_of(' ');
      extra_guidance = start == std::string::npos ? "" : extra_guidance.substr(start);
      return handle_ai_eval_command(evaluation_worker, project_id, first_token, extra_guidance);
    }
    // 没有范围标记:整段 rest 就是对当前图片的额外指引
    bool accepted = evaluation_worker.request(current_image_id, pzt::core::Provider::Gemini, rest);
    if (!accepted) return pzt::cli::i18n::msg_ai_processing_pending();
    // ...提交成功的确认反馈,照抄原来"裸文本提交"那条路径的做法(闪一下、
    // 800ms、返回空字符串)，只是搬到这个分支里
  }
  return pzt::cli::i18n::msg_ai_unknown_command(command);
}
```

`handle_ai_eval_command`：range 解析交给 `resolve_console_scope`，拿到图片列表之后逐张查 `get_image(id)->evaluation`，已经有值的跳过，没有的调 `evaluation_worker.request(...)`，最后汇总提交了多少张：

```cpp
std::string handle_ai_eval_command(pzt::core::EvaluationWorker& evaluation_worker,
                                    pzt::core::ProjectId project_id, const std::string& scope,
                                    const std::string& extra_guidance) {
  auto resolved = resolve_console_scope(project_id, scope);
  if (!resolved.error_message.empty()) return resolved.error_message;

  int submitted = 0;
  for (auto id : resolved.image_ids) {
    auto info = pzt::core::get_image(id);
    if (info && info->evaluation) continue;  // 已经评估过,跳过
    if (evaluation_worker.request(id, pzt::core::Provider::Gemini, extra_guidance)) ++submitted;
  }
  return pzt::cli::i18n::msg_ai_eval_submitted(submitted);
}
```

`handle_tasks_command` 直接读 `EvaluationWorker::queue_status()`，拼成一句状态文案，不需要额外逻辑。

**性能提醒**：`handle_ai_eval_command` 对列表里每一张图各调一次 `pzt::core::get_image(id)`（每次内部 `open_default()` 开一个新连接），项目里图片数量大的时候这是 N 次数据库往返，不是一次批量查询——这次先这么写，量级（几百到几千张）预计不是瓶颈，如果真机测试发现明显卡顿，再考虑加一个批量版本的"哪些图片已经评估过"查询（比如 `SELECT image_id FROM image_evaluations WHERE image_id IN (...)`），不提前做这个优化。

### `cli/i18n`：字符串跟着新形状调整

`ai_score_label`/`ai_score_comment_text`/相关的 "AI Score"/"AI Comment" 字符串这次全部换掉，改成描述三个维度+达标状态的新字符串集合（具体字符串名称/文案留给实现阶段，跟上一版一样遵循"一个函数一个字符串，`if (g_lang==zh)` 分支"的既有惯例）。

`menu_item(":", "AI 控制台")` **改成了** `menu_item(":", "控制台")`（英文同理，"AI Console"→"Console"）——这一版加了 `/dedup` 之后，这个入口不再是纯 AI 相关的功能了（近似重复检测是本地算法，不调用任何云端模型），继续叫"AI 控制台"会造成误导。

新增：`msg_console_requires_slash()`（空输入或非 `/` 开头的输入统一提示）、`msg_ai_unknown_command(command)`（带上具体命令名）、`err_console_tag_not_found(tag_name)`（`/dedup`/`/ai_eval` 共用）、`err_console_invalid_scope()`（范围既不是 `*` 也不以 `#` 开头）、`msg_ai_eval_submitted(count)`（批量提交了几张，`count` 为 0 时文案自然表达"没有需要处理的"）、`msg_ai_tasks_status(queued, processing)`（`/tasks` 的状态文案，`processing` 决定要不要多一句"有一个正在处理中"）。`msg_ai_prompt_placeholder()` 的文案也从"输入额外指引"改成直接列出几种命令写法，因为控制台不再有"裸文本"这条隐藏路径，占位提示得教会用户新的输入方式。

## 技术选型（不变）

curl+nlohmann_json 已经接线完成，CMake 配置不用动。API key 走环境变量的决定不变。

## 任务分解

上一版任务 1（CMake 接线）、2（`request_json` 通用层）已经完成且这次不需要改，不再重复列出。以下 1-5 已完成并提交（Gemini 真机验收过；Claude 手头没有 key，还没跑，见"多供应商切换"那条 TODO）：

1. ~~**Schema 迁移**：`image_evaluations` 新表 + `images` 表删掉旧四列~~
2. ~~**`core/ai/evaluation.h`/`.cpp`**（取代 `score.h`）~~
3. ~~**`core/ai/evaluation_worker.h`/`.cpp`**（取代 `score_worker.h`）~~
4. ~~**CLI 接线**：`ScoreWorker`→`EvaluationWorker` 改名、信息栏展示重做、i18n 字符串~~
5. ~~**真机验收**（Gemini）~~

这次新增（`/ai_eval`、`/tasks` 批量命令），已完成并提交：

6. ~~**`EvaluationWorker::queue_status()`**：新增聚合状态查询~~——单元测试覆盖三种状态（全空闲、只有一个在处理、一个在处理+有积压排队）下 `queued`/`processing` 的值是否正确。原设计写的是"空队列"、"有排队没在处理"、"有排队且有一个在处理"，实现时发现"有排队但没在处理"在单 worker 线程模型下只存在于请求刚提交、worker 线程还没被调度醒来的极短窗口内，没法确定性复现，测试计划相应调整为上述三种真正稳定可测的状态
7. ~~**`cli/commands/browse.cpp`**：`handle_ai_prompt_flow` 改成强制要求 `/` 前缀、`/` 分支解析（`handle_ai_console_command`/`resolve_console_scope`/`handle_ai_eval_command`/`handle_tasks_command`）、新增 i18n 字符串~~——沿用"cbreak 按键循环没法自动化测试"的一贯限制，`handle_ai_eval_command` 内部调用的 `list_images`/`find_tag_by_name`/`filter_by_tag`/`get_image` 都是已经测过的现成函数，这次不需要为编排逻辑本身另开单元测试，靠真机验证命令解析和批量提交的行为
8. ~~**真机验收**~~：`/ai_eval *`、`/ai_eval #标签名`、`/ai_eval [额外指引]`（当前图片）、`/tasks`、空输入/非 `/` 开头输入的拒绝提示，都用 pty 驱动一个真实 `pzt open` 会话跑过一遍，确认跳过已评估图片的行为正确、`/tasks` 的数字反映真实队列状态、未知命令有清楚提示、误触发风险确实被堵上（真实网络调用因为没有可用的 API key 没有覆盖，机制本身——提交/去重/落库——在这之前的 Gemini 真机验收里已经跑通）

## 风险与待确认问题

延续自 `docs/M3_PRD.md`（云端 vs 本地、自动介入触发条件、近似重复检测独立立项、自动打标签、多供应商切换），这次工程设计阶段新增：

* **修正建议的精度**：旋转角度、裁切百分比这类精确几何量，模型给出的值靠不靠谱这次没有把握，真机验收阶段重点观察，不承诺精度
* ~~**`DROP COLUMN` 的 SQLite 版本兼容性**~~：已确认，`cmake` 实际链接的是 macOS 系统 SQLite 3.43.2，远高于 3.35.0 的门槛，不需要兼容代码
* **`schema_instruction` 用自然语言而不是原生结构化输出**：跟上一版一样的顾虑，如果真机测试发现 `ParseError` 频繁（这次 JSON 形状比上一版复杂不少，出错概率可能更高），需要重新评估要不要升级成 Claude `tool_use`/Gemini `responseSchema`
* **`/ai_eval` 逐张查 `get_image` 的性能**：见"批量提交"一节，N 次数据库往返没有预先优化，量级大时可能需要换成批量查询
* **`/ai_eval #标签名` 的解析边界、额外指引恰好以 `#` 开头的歧义**：见 `docs/M3_PRD.md` 同一条风险的说明——`#` 前缀方案是实现阶段为了消解"批量 vs 当前图片"歧义新引入的，连带引入了这个新的边界情况
