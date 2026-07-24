# PicZTream (PZT) 工程设计文档（W2026-07-21 目标一：eval 解耦 + core pairwise 地基）

> **已归档(2026-07-24)**：目标一「eval 解耦 + core pairwise 地基」已完成，落地与本文档一致。`overall_score`/`passes_gate` 已全面移除（除解释性注释），`unusable` flag 落在 `core/ai/evaluation.h`/`compare.cpp`/`evaluation_worker.cpp`，`pzt open` 信息栏与 `pzt compare` 均已接上。本周开发目标全貌见 `docs/W2026-07-21_PRD.md`。

## 背景

本文档是 `docs/W2026-07-21_PRD.md` 两阶段方案的第一份 Eng Design，覆盖目标一（地基）。目标二（dedup 两类 / curate 两模式 / 全局 AI 开关 / agent 锦标赛编排）另出 `W2026-07-21_Tournament_Eng_Design.md`，不在本文范围。

PRD 的总纲：**AI eval = 客观评价当前单张照片**（产文字描述 + 硬伤 flag，不再产任何跨图可比分数）；**涉及比较的选择 = 锦标赛**（两两会话内视觉比较，不是单张打分再横向比）。本阶段把地基铺好：

1. eval 输出从三维分数（exposure/composition/focus 各 0-10 + note + 修正建议 + comment）改成"一段客观文字 `assessment` + 一个 `unusable` 硬伤 flag"。
2. auto-reject 判据从 `passes_gate`（三维达标）改成 `unusable` flag。
3. 移除 `overall_score`/`passes_gate`，把当场断掉的两个消费者（dedup 的 keep 选择、curate 的候选过滤与质量排序）降级到非 AI 基线。
4. 新建 core pairwise 视觉比较能力（`core/ai/compare`）+ headless 命令。**本阶段只建好并单测/真机验证，不接入任何锦标赛**——锦标赛 bracket 编排按 PRD 拍板落在 agent（目标二）。

**eval 的定位（2026-07-21 补充，收紧）**：eval 从此只服务 pzt CLI 人类用户的**手动触发查看**（`pzt open` 控制台 `/ai_eval`、headless `pzt eval`），**agent 不再对整批照片跑 eval**——太慢，且它对选片的唯一贡献是排除废片，而废片我们假定在锦标赛里也会被淘汰。这条推论进一步解耦 curate/dedup 与 eval：它们的"默认排除废片"只看**废片标签**（标签可能来自手动 eval 的 auto-reject，也可能来自用户手动打标），不依赖任何 eval 记录，因此本阶段 curate/dedup 的候选/保留逻辑对 evaluation 记录**零依赖**（见决策四、五）。eval 的产生机制（assessment+flag、auto-reject、批量 `/ai_eval *`、headless `pzt eval --auto-reject`）全部保留，只是不再被 agent 整批调用。agent 侧删除整批 Evaluate stage、全局开关不再门控 eval 的设计属于目标二。

这套改动是破坏性的：`overall_score`（三维平均，`core/ai/evaluation.cpp:153-157`）和 `passes_gate`（三维都 ≥6，`evaluation.cpp:159-163`）是这套分数对下游的唯一出口，删掉它们必然连带改 dedup/curate，因此第 5、6 节的降级与第 3、4 节的 eval 重构是同一个不可分割的提交批次。

## 现有代码基础（复用，不重造）

- **`core/ai/ai.h`/`.cpp` 的 `request_json`**（`ai.cpp:364-440`）：通用发图层，不知道 "evaluation"/"style" 是什么，只负责"发图 + 要 JSON + 原样交还"。**当前写死单图**——签名收单个 `const decode::DecodedImage&`（`ai.h:67`），内部编码成单个 `image_base64`（`ai.cpp:383`），三个 provider builder（`build_claude_request`/`build_gemini_request`/`build_local_request`，`ai.cpp:115/150/186`）都只接一个 base64、位置写死。pairwise 要发两张图，这一层需要扩展（见第 6 节）。
- **`core/ai/style.h`/`.cpp`**：`request_json` 的另一个视觉消费者（看图选风格），是"新增一个 core/ai 视觉能力"最贴的先例。`compare` 照抄它的骨架：结果结构体 + 错误枚举（含幻觉保护 `Hallucinated`，`style.h:17`）+ prompt/schema_instruction/json_schema 三段 builder + `detail::` 带 `http_post` 注入缝的 impl（`style.cpp:87-117`）+ 默认注入 `perform_curl_post` 的公开壳。
- **`cli/commands/commands.cpp` 的 `recipe_suggest`**（`commands.cpp:1073-1131`）：调一次 core/ai 视觉能力的 headless 命令模板。`compare` 命令照抄：手写 flag 循环解析 `--json`/`--provider`、`resolve_project_json`、`find_image_by_path` → `decode_image_for_ai(ImageId)`（`commands.cpp:1038-1049`，注意收 ImageId 非路径）、`emit_json`/`emit_json_error` + 稳定机读错误串。
- **`core/db/schema.cpp` 的 `column_exists`**（`schema.cpp:137-153`）：`PRAGMA table_info` 判列存在，schema 重建的幂等判据复用它。注意仓库现在**没有**活的 DROP COLUMN 代码（F-33 那个 `ensure_column_dropped` helper 已删，`schema.cpp:197-205` 只剩注释）。
- **`core/curate/curate.cpp` 的 `greedy_pick` farthest-point 多样性机制**（`curate.cpp:92-139`）：时间维度的"离已选集最远"贪心，正是"找尽量不一样的图片"。降级时保留这套机制，只摘掉质量分档。
- **`core/tagging` 的 `ensure_reject_tag`/`add_tag`**（`tagging.h:131/82`）：auto_reject 打废片标签的链路，判据换了但链路不动。

## 数据形状与 schema 设计

### `core/ai/evaluation.h`：新结构体

```cpp
namespace pzt::core::ai {

enum class EvaluationError { MissingApiKey, NetworkError, HttpError, ParseError,
                              ImageUnavailable, StorageFailed };  // 删 OutOfRange

struct EvaluationResult {   // 纯模型输出
  std::string assessment;   // 一段简练客观文字,覆盖构图/色彩/对焦/摄影审美
  bool unusable;            // 是否存在硬伤导致根本不可用
};

struct EvaluationInfo {     // 落库/读回,多两个发起时已知的上下文字段
  std::string assessment;
  bool unusable;
  std::string extra_guidance;
  std::string provider;
};

// 取代 passes_gate:直接读模型给的 flag,不再从分数算 gate。
// core/api.h 别名透传,给 browse 信息栏等展示类共用一处,不各自写 !unusable。
bool is_usable(const EvaluationInfo& info);   // return !info.unusable;

}  // namespace pzt::core::ai
```

- `assessment` 是**单一整体文字 blob**，不拆成构图/色彩/对焦/审美的分字段——PRD 定的是"一个整体的评价"。展示、未来检索都消费这一段，不做结构化。
- 删除 `DimensionAssessment`/`ExposureFix`/`CompositionFix`（`evaluation.h:31-46`）。删除 `overall_score`/`passes_gate`（声明 `evaluation.h:82-83`、定义 `evaluation.cpp:153-163`）及 `core/api.h:93-94` 的 `using` 别名，换成 `is_usable` 的别名。`kEvaluationGateThreshold`（`evaluation.h:80`）随 `passes_gate` 一起删。
- `is_usable` 的调用方只剩 `pzt open` 信息栏的可用性标记（`browse.cpp:401`）和展示类——**curate/dedup 已完全不看 evaluation 记录**（见决策四、五的收紧），不再复用它。
- `EvaluationError` 删 `OutOfRange`（分数没了、0-10 越界校验消失），其余保留。`map_request_error`（`evaluation.cpp:88-100`）不变。

### `core/ai/evaluation.cpp`：prompt / schema / 解析

- `build_evaluation_prompt(extra_guidance)`（`evaluation.cpp:14-23`）重写：要求模型产出一段简练客观的文字，从构图、色彩、对焦、摄影审美几个方面描述这张照片；外加判定 `unusable`——是否存在某方面硬伤（严重失焦、严重欠/过曝等）导致这张根本不可用。`extra_guidance` 非空时仍追加 `"\n\nAdditional guidance: ..."`。模板固定英文，不跟 i18n。
  **修订（2026-07-22）：判据收紧。** 真机测试发现一张明显失焦、无意义构图的"膝盖照"被判 `usable`——旧措辞要求"badly"级失焦+"no recoverable detail"级曝光,门槛太高。改成:对焦判据从"灾难级模糊"降到"不足以当清晰可用的照片";新增"意外/无意义构图"(无可辨识主体,如误拍身体部位/口袋/地面)一条,覆盖技术上不算爆炸但根本不成立为一张照片的情况;并显式提示模型"不需要严重到灾难级才算 unusable,只要不值得留"。仍是 PRD 定义的"硬伤"技术/实用性判据范畴,不是转向宽泛审美评分。
- `build_evaluation_schema_instruction`（`evaluation.cpp:25-40`）+ `build_evaluation_json_schema`（`evaluation.cpp:42-86`）重写成 `{ "assessment": string, "unusable": boolean }`，两字段都 `required`。native json_schema 供 Local 硬约束解码（走现有 `build_local_request` 的 `options.temperature=0` 路径，`ai.cpp:203`）。
- `request_evaluation_impl`（`evaluation.cpp:167-206`）：删 `parse_dimension`/`parse_exposure_fix`/`parse_composition_fix`（`evaluation.cpp:105-149`）及越界检查。新解析：`assessment` 必须是 string、`unusable` 必须是 boolean，任一缺失/类型不对 → `ParseError`。`request_evaluation` 公开签名不变（`image, extra_guidance, provider, local_config`，`evaluation.h:100-103`），继续当 `EvaluationFn` 默认值。

### `core/db/schema.cpp`：整表重建

新 `kCreateImageEvaluations`（`schema.cpp:112-131` 替换）：

```sql
CREATE TABLE IF NOT EXISTS image_evaluations (
  image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
  assessment TEXT NOT NULL,
  unusable INTEGER NOT NULL,       -- 0/1
  extra_guidance TEXT NOT NULL,
  provider TEXT NOT NULL
);
```

幂等一次性重建（`initialize_schema`，`schema.cpp:164` 一带，CREATE 之前插一句）：

```cpp
if (column_exists(conn, "image_evaluations", "exposure_score")) {
  exec(conn, "DROP TABLE image_evaluations");   // 老 schema:整表 drop
}
// 随后 CREATE TABLE IF NOT EXISTS 建新表;重建后 exposure_score 不存在,不再 drop,幂等
```

这次的重建是**破坏性的**：首次跑到新版会清空 `image_evaluations` 全部现有行（含真机上迭代累积的评估）。PRD 已拍板"库里都是测试数据、直接作废重设、不写迁移"，评估可重跑，接受。之所以整表 drop 而非逐列 surgery：这次是 11 列删 + 2 列增的大改形，且无数据要保，`DROP TABLE` + `CREATE` 比 11 条 `DROP COLUMN`（还要现补一个 helper）干净得多。

`get_image` 的 `LEFT JOIN image_evaluations`（`core/project/project.cpp` 内）取列改成新四列，`EvaluationInfo` 整行填好或整行 `nullopt`（这张表要么整行在要么整行不在的语义不变）。

**修订（2026-07-22）：`assessment`/`unusable` 两列再合并成一列 `result_json`。**

```sql
CREATE TABLE IF NOT EXISTS image_evaluations (
  image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
  result_json TEXT NOT NULL,       -- 模型原始返回:{"assessment":"...","unusable":true|false}
  extra_guidance TEXT NOT NULL,
  provider TEXT NOT NULL
);
```

动机：`assessment`/`unusable` 都是"问 AI 要的值"，以后想再让模型多给一个类似的值（比如再加一个软性信号），不该每次都要一次破坏性表重建——把它们收进一个 JSON blob，未来加字段只需要扩展 `EvaluationResult`（`evaluation.h`）+ worker 落库那行 json 字面量 + `get_image` 解析那几行，schema 本身不用再动。`extra_guidance`/`provider` **不进 blob**，继续独立列——它们不是模型输出，是调用方自己知道的请求上下文（谁问的、怎么问的），跟"AI 返回了什么"是两类东西，混进同一个 blob 会模糊这个边界，且这两个字段目前看不到会跟着模型 schema 一起演化的理由。

迁移条件相应加宽（`unusable`/`exposure_score` 任一旧列存在就整表 drop，表不存在时两次 `column_exists` 都是 false、不误触发）：

```cpp
if (column_exists(conn, "image_evaluations", "exposure_score") ||
    column_exists(conn, "image_evaluations", "unusable")) {
  exec(conn, "DROP TABLE image_evaluations");
}
```

`get_image` 读回时用 `nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false)` 非抛出式解析；解析失败或缺字段/类型不对，按"没评估过"处理（`info.evaluation` 留空），不阻塞——这是缓存性质的 AI 结果，不是权威数据，跟"评估可重算、不是金标准"的既有口径一致。`EvaluationInfo`/`EvaluationResult` 两个 C++ 结构体本身不变，只有落库/读回这两处收口了序列化。

## worker 落库 + auto_reject（`core/ai/evaluation_worker.cpp`）

- 成功落库的 UPSERT（`evaluation_worker.cpp:125-187`）从 16 列缩到 4 列：`image_id, result_json, extra_guidance, provider`；`result_json` 是 `nlohmann::json{{"assessment", r.assessment}, {"unusable", r.unusable}}.dump()`，`provider` 仍走 `to_string(req.provider)`。`ON CONFLICT(image_id) DO UPDATE` 结构不变（2026-07-22 修订：原计划是 5 列，`assessment`/`unusable` 分列，后收进 `result_json` 一列，见上）。
- auto_reject（`evaluation_worker.cpp:195-203`）：判据从 `!passes_gate(eval_info)` 改成直接 `if (r.unusable)`——模型直接给可用性，不再构造 `EvaluationInfo` 去算 gate。命中后 `ensure_reject_tag(db, project_id)` + `add_tag`，用已开的 `db` 连接，只打不删，全不变。
- `EvaluationFn` typedef（`evaluation_worker.h:27-28`，返回 `EvaluationResult`）、`request()` 签名（含 M4 加的 `auto_reject` bool，`evaluation_worker.h:49-50`）、队列/去重 `in_flight_`/`queue_status`/`take_last_failure` 骨架全不动。失败路径（`ImageUnavailable`/`StorageFailed`/不清旧行）不变。

## dedup 降级（`core/dedup/dedup.cpp`）

`pick_keep_id`（`dedup.cpp:107-162`）是 dedup 里唯一碰分数的地方（唯一 `overall_score` 调用在 `dedup.cpp:130`）。降级：

- 删掉"判定全员评估过 → 按 `overall_score` 三级择优"整段（`dedup.cpp:109-149`），保留原来"未全员评估"的退化分支（`dedup.cpp:150-160`）作为**唯一路径**：keep 恒为 `captured_at` 最新，打平 `id` 最小兜底。
- 删 `#include "core/ai/evaluation.h"`（`dedup.cpp:11`）、删逐图 `get_image().evaluation` 查库（`dedup.cpp:113-114`）。`pick_keep_id` 签名可简化去掉 `db`（不再查库），只收 `cluster` + `members`；`find_duplicates_impl` 的调用点（`dedup.cpp:340`）同步改。
- 更新头注释（`dedup.h:64-72`、`dedup.cpp:104-106`）：删掉"全员评估按分数留"的描述。`DuplicateGroup`/`DedupSummary`/`find_duplicates` 签名与聚类/并查集/打标签逻辑全不动。

**新增：dedup 默认排除废片**（2026-07-21 补充）。现状 `find_and_tag_duplicates`（`dedup.cpp:206-252`）在传入的 image_ids 全量上聚类，**并不排除废片**——keep 改成"留最新"后，一张废片若 `captured_at` 最新会在簇内成为 keep、把它的好邻居打成重复。改：`find_and_tag_duplicates` 在调 `find_duplicates` 之前，先用 `tagging::find_tag_by_name(kRejectTagName)`+`tagging::images_with_tag`（跟 curate 排除废片同一套）从 image_ids 里摘掉废片，只在剩余图上聚类。"清旧重复标记"一步仍作用于传入的完整 scope（避免废片上残留旧的重复标签），只有"聚类"这一步走废片-排除后的子集。这样废片既不会成为 keep、也不会把好图挤成重复。**注意这是本阶段对 dedup 的一处行为新增（现状无此排除），不是"保留现状"**，与 curate 的废片排除对齐；与之对应，`DedupSummary` 的语义不变（group/tagged/skipped_no_capture_time 都只统计参与聚类的子集）。

这个"留最新 + 排废片"正是 PRD 目标二"非 AI dedup"要的行为，提前在地基落定。

## curate 降级（`core/curate/curate.cpp`）

curate 有三处依赖分数：候选过滤（`passes_gate`，`curate.cpp:46`）、代表打分（`overall_score`，`curate.cpp:84`）、簇数<N 排序（`RepInfo::score`，`curate.cpp:177`）。降级：

- **候选规则**（`resolve_candidates`，`curate.cpp:43-47`）：**改成纯标签排除，彻底不看 evaluation 记录**（2026-07-21 收紧）。废片标签（`kRejectTagName`）+ 重复标签（`kDuplicateTagName`）的排除两段（`curate.cpp:33-40`）保留，删掉原来逐图 `get_image().evaluation` + `passes_gate` 的判定（`curate.cpp:45-46`）：
  ```cpp
  // resolve_candidates 尾部:排除集只来自标签,不再逐图查 evaluation
  for (auto id : ids) {
    if (excluded.count(id)) continue;   // excluded = 废片 ∪ 重复
    candidates.push_back(id);
  }
  ```
  即 `候选 = 非废片 非重复`。理由：eval 不再整批跑（见背景补充），绝大多数图没有 evaluation 记录，curate 不能依赖它；废片排除靠标签（来自手动 eval 的 auto-reject 或用户手动打标）就够，废片以外的次品假定在锦标赛里淘汰。**行为变化**：未评估的图进候选（原规则排除未评估）；评估为 `unusable` 但没被打废片标签的图也进候选（原设计想用 unusable 做双保险，现按简化去掉——真要排它，auto-reject 打上废片标签即可）。附带好处：`resolve_candidates` 不再对每张图调 `get_image`（原 `curate.cpp:45`），省 N 次库往返；`curate.cpp` 删掉 `#include "core/ai/evaluation.h"`（`curate.cpp:8`），curate 完全与 AI 解耦。
- **去掉质量分**：`RepInfo`（`curate.cpp:76-80`）删 `score` 字段 → `{id, captured_at}`；`make_rep_info`（`curate.cpp:82-85`）删 `overall_score` 调用 → `{id, captured_at}`。
- **选择退化为纯时间多样性**：`greedy_pick`（`curate.cpp:92-139`）删掉"求最高 score / 收集同分 tied"档（`curate.cpp:93-99`），所有代表视为等价，farthest-point 从第一张就生效——seed 选 `captured_at` 最新（id 兜底），之后每步选时间上离已选集最远者（保留 `curate.cpp:102-134` 的 farthest-point 计算，只是不再被 score 门槛前置过滤）。簇数<N 排序（`curate.cpp:176-182`）从 `a.score > b.score` 改成按 `captured_at` 降序、id 兜底。
- `build_cluster_reps`（`curate.cpp:59-74`）不动：它靠 dedup `find_duplicates` 的 keep_id 取代表，keep_id 已在上一节改成时间基准，一致。curate 的粗簇参数仍从 `Settings.curate_time_window_seconds`(20)/`curate_hash_threshold`(10) 传入（本就比 dedup 的 10/5 粗，`settings.h:34-35`）。

## core pairwise 比较能力

### `core/ai/ai.{h,cpp}`：多图扩展（最小涟漪）

现状三个 builder 写死单图。改法保证现有单图调用方（evaluation.cpp/style.cpp）零改动：

- 三个 `build_*_request` 内部图片参数从 `const std::string& image_base64` 改成 `const std::vector<std::string>& image_base64s`：Claude 在 `messages[0].content` 数组按序插 N 个 `{type:image,...}` 再跟 text（`ai.cpp:115-131`）；Gemini 在 `contents[0].parts` 按序插 N 个 `{inline_data:...}` 再跟 text（`ai.cpp:150-161`）；Local 的 `messages[0].images` 本就是数组，直接放 N 个 base64（`ai.cpp:195`）。
- 新增重载 `request_json(const std::vector<decode::DecodedImage>& images, ...)`（其余参数同现签名），内部对每张 `downscale_for_upload` + `encode_jpeg_bytes` + `base64_encode` 组成 `image_base64s`。现有 `request_json(const DecodedImage&, ...)` 改成塞 1 元素 vector 转调新重载的薄壳——`ai.h:67-73` 单图签名保留，evaluation/style 不用改一行。

### `core/ai/compare.{h,cpp}`（新文件，照抄 style.cpp 模式）

```cpp
namespace pzt::core::ai {

enum class CompareError { MissingApiKey, NetworkError, HttpError, ParseError, InvalidWinner };

struct ComparisonResult {
  int winner;              // 0 => a 更好, 1 => b 更好
  std::string reasoning;   // 一句简短理由
};

// 把 a、b 两张图放进同一次会话做视觉比较,必须二选一。
Result<ComparisonResult, CompareError> request_comparison(
    const decode::DecodedImage& a, const decode::DecodedImage& b,
    Provider provider, const LocalModelConfig& local_config = LocalModelConfig{});

namespace detail {
Result<ComparisonResult, CompareError> request_comparison_impl(
    const decode::DecodedImage& a, const decode::DecodedImage& b,
    Provider provider, HttpPostFn http_post,
    const LocalModelConfig& local_config = LocalModelConfig{});
}

}  // namespace pzt::core::ai
```

- prompt（`build_compare_prompt`）：给两张标为 a、b 的照片，从**构图、色彩、摄影审美**综合比较，**必须选出更好的那一张，不允许平局**（bracket 需要确定晋级；对焦这类可用性问题由 eval 的 `unusable` 在上游过滤，pairwise 假定两张都可用，只比相对好坏）。
- schema_instruction + json_schema：`{ "winner": "a"|"b", "reasoning": string }`，`winner` 用 enum `["a","b"]` 硬约束（Local）。
- 解析：`winner` 是 "a" → 0、"b" → 1，其它值 → `CompareError::InvalidWinner`（幻觉保护，对应 style 的 `Hallucinated` 校验，`style.cpp:105-113`）。`request_comparison_impl` 调新的 `request_json(std::vector{a,b}, ...)`，`map_request_error` 同 style。

### headless 命令 `pzt compare`（`cli/commands/commands.cpp` + `cli/main.cpp`）

`pzt compare <project> <pathA> <pathB> --provider <gemini|claude|local> --json`，照 `recipe_suggest` 模板：

- 解析 `--json`（必需）/`--provider`/三个 positional（project + 两个路径）。
- `resolve_project_json(project)` → 两次 `find_image_by_path` 拿两个 `ImageId` → 两次 `decode_image_for_ai(ImageId)`（`commands.cpp:1038-1049`，走 `decode_preview_file`）→ `request_comparison(a, b, provider, local_config)`。
- 成功 `emit_json({{"winner", 胜者的原始 path}, {"reasoning", ...}})`（winner=0 回 pathA、1 回 pathB，机读侧直接拿到胜者路径，不用自己映射 a/b）。失败经新 `compare_error_str(CompareError)` 稳定串 + `emit_json_error`。
- `cli/main.cpp` 注册 `compare` 分发。

**本命令本阶段不被任何锦标赛调用**——目标二 agent 侧才用它逐对驱动 bracket。地基只保证命令能对两张真实图跑通、返回确定胜者。

## CLI 展示与 headless 输出收口

- **`pzt open` 信息栏**（`browse.cpp:960-1005`）：原三维分数 + 达标 + 三行维度 + comment，换成——状态行（可用/硬伤，新 `evaluation_status_label(bool unusable)`）+ `wrap_text(eval.assessment, info_cols)` 若干行。保留 `evaluation_none_label`（未评估）。严守既有三约束（`browse.cpp:820-857` 注释）：整块拼进一个 `out` 一次 `write_stdout`、`[image_top_row, meta_bottom_row)` 先全范围 `pad_to("")` 清空、`emit_line` 越界裁剪。因为新展示行数比原来少，跨帧残影风险更低，但清空逻辑照旧不能省。
- `browse.cpp:401`（浏览态标记）：`info->evaluation.has_value() && !passes_gate(...)` → `info->evaluation.has_value() && !is_usable(*info->evaluation)`（即 `unusable`）。
- **i18n**（`i18n.cpp:947-997`）：删 `evaluation_summary_label`/`evaluation_exposure_line`/`evaluation_composition_line`/`evaluation_focus_line` 及其声明（`i18n.h:232-239`），新增 `evaluation_status_label(bool unusable)`（zh 如"选片评估: 可用 | 有硬伤"，en "Culling: usable | unusable"）。`compare` 是纯 headless JSON，不需 i18n 文案。
- **`cmd_eval` 输出**（`commands.cpp:403-421`）：`evaluated` 数组每项从 `{path, passes_gate, overall_score}` 改成 `{path, unusable}`；`submitted`/`failed` 不变。`PZT_FAKE_EVAL` 的假 `EvaluationFn`（`commands.cpp:361-380`，原产 8/8/8）改产新形状 `{assessment:"fake", unusable:false}`。
- **`cmd_images` 输出**（`commands.cpp:539-540`）：每图 `{passes_gate, overall_score}` 改成 `{unusable}`（未评估的图该字段缺省/null，同现有 evaluation 缺省处理）。

## 测试

- **`core/tests/evaluation_test.cpp`**：重写为新形状——`assessment`/`unusable` 正常解析、缺字段或类型错 → `ParseError`、`unusable=true/false` 都能读、guidance 拼接进 prompt、Local 路径传了 native json_schema。删 `OutOfRange`（`:172`）、三维 fix（`:102-155`）、`overall_score`/`passes_gate`（`:305-316`）相关用例。RequestError→EvaluationError 映射、MissingApiKey 不调 http_post 保留。
- **`core/tests/evaluation_worker_test.cpp`**：`make_evaluation_result` 辅助改产 `{assessment, unusable}`；落库用例通过 `get_image()` 读回验证（不直接断言列数/列名，跟存储格式解耦——2026-07-22 起落库是 `result_json` 一列，见上）；auto_reject 三例（`:231/:249/:268`）改成 `unusable=true`+auto_reject → 打废片、`unusable=false`+auto_reject → 不打、auto_reject=false → 不打。StorageFailed（`:291`）、queue_status（`:380`）、in-flight 去重（`:120`）、last_failure（`:190/:211`）、LocalModelConfig 透传（`:473`）保留。
- **`core/tests/dedup_test.cpp`**：依赖 `overall_score` 的 keep 用例（`:279` 全评估按分选、`:297` 分数打平按时间、`:315` 部分未评估退化）重写为"keep 恒为 captured_at 最新、id 兜底"；`insert_evaluation` 辅助（`:107`）可删（keep 不再看评估）。**新增用例**：带废片标签（`kRejectTagName`）的图被排除在聚类之外——构造"一张废片 + 它的好近邻"，验证废片不成为 keep、好图不被打成重复。聚类/hash/并查集/标签/summary 其余用例不变。
- **`core/tests/curate_test.cpp`**：候选用例改成纯标签口径——未评估的图进候选、评估为 `unusable` 但未打废片标签的图也进候选（不再按 unusable 排除）、废片/重复标签排除保留（`:118/:130/:141` 相应重写；原 gate 排除用例 `:130` 删除或改成"打了废片标签才排除"）。排序用例（`:166` 每簇一代表、`:194` 打平时间铺开、`:237/:257`）改成纯 `captured_at` 多样性口径。`insert_evaluation`（`:89`）在 curate_test 里基本不再需要（curate 不看评估），候选测试改用标签建场景。
- **`core/tests/compare_test.cpp`**（新，镜像 `style_test`）：winner "a"/"b" 解析成 0/1、非法 winner → `InvalidWinner`、prompt 含两张图指示、RequestError→CompareError 映射、MissingApiKey 不调 http_post。
- **`core/tests/ai_test.cpp`（或对应）多图**：新增用例验证 `request_json(std::vector{img1,img2}, ...)` 对 Claude/Gemini/Local 三个 provider 各自把两张图正确编码进请求体（content/parts 两个 image 元素、Local images 数组两元素），复用 `HttpPostFn` 注入捕获 body 断言，不连网络。

## 任务分解（TDD RED→GREEN，一个可提交单元一 commit；按"每 commit 可构建"排序）

删 `overall_score`/`passes_gate` 之前必须先让 dedup/curate 不再调它们，否则中间提交编译不过——因此把下游解耦排在 eval 结构重构**之前**。三个单元里 1→2 有此顺序约束，3 独立可并行。

1. **dedup/curate 脱离分数**（eval 老结构不动，`overall_score`/`passes_gate` 仍在、只是无人从 dedup/curate 调，全程可构建）：
   - dedup：`pick_keep_id` 降级为留 `captured_at` 最新（删 `overall_score`/`evaluation` 依赖、签名去 `db`、删 `#include evaluation.h`、更新头注释）；`find_and_tag_duplicates` 聚类前排废片。
   - curate：`resolve_candidates` 改纯标签排除（删 `passes_gate`/逐图 `get_image`、删 `#include evaluation.h`）；`RepInfo`/`make_rep_info`/`greedy_pick`/簇数<N 排序去质量分、改纯 `captured_at` 时间多样性。
   - 测试：`dedup_test`（keep 用例重写为留最新 + 新增排废片用例）；`curate_test`（候选纯标签、排序时间多样性；移除只为 keep/候选服务的 `insert_evaluation` 依赖）。
2. **eval 重构 + 删分数函数 + 展示收口**（原子：删函数与最后的展示消费者同一提交才可构建）：`evaluation.h`/`.cpp` 新结构体 + prompt/schema + 解析 + 删 `overall_score`/`passes_gate`/`kEvaluationGateThreshold` + 加 `is_usable` + `EvaluationError` 删 `OutOfRange`；`api.h` 别名换 `is_usable`；`schema.cpp` 整表幂等重建；`project.cpp` join 取新列；`evaluation_worker.cpp` 5 列 UPSERT + auto_reject 改 `unusable`；`browse.cpp` 信息栏 + `:401`；`i18n` 增删；`cmd_eval`/`cmd_images`/`PZT_FAKE_EVAL` 输出。配套 `evaluation_test`/`evaluation_worker_test` 重写。
3. **pairwise**（独立，可与 1/2 并行，不碰分数）：`ai.{h,cpp}` 多图扩展 + 多图测试；`compare.{h,cpp}` + `compare_test`；`pzt compare` headless + `main.cpp` 注册。

## 验证

- `core/tests` 全绿（含新 `compare_test`、多图用例、重写的 evaluation/dedup/curate/worker 用例）。
- **同时构建 Debug（`build/`）与 Release（`build_release/`）**——用户用 release 二进制真机测。
- 真机（Local/Ollama）：
  - `pzt eval --provider local --scope * --auto-reject` 产出 `{path, unusable}`，`unusable=true` 的图被打上废片标签。
  - `pzt open` 信息栏显示文字 `assessment` + 可用/硬伤状态，不再出现三维分数。
  - `pzt compare <proj> <a> <b> --provider local --json` 返回确定的 `{winner: <path>, reasoning}`。
  - `pzt dedup --scope *` 每组保留 `captured_at` 最新的那张；带废片标签的图被排除、不成为 keep、不把好邻居打成重复。
  - `pzt curate --count N` 选出 N 张，纯标签排除（未评估图进候选、废片/重复排除），选择呈现时间多样性。

## 风险与待确认

- **schema 整表 drop 的破坏性**：首次跑新版清空全部现有评估（真机上累积的），PRD 已接受，此处仅重申——用户若在切版前有想留的评估结果需自行导出，本阶段不提供迁移。
- **`unusable` 判据的稳定性**：模型自主判"是否硬伤不可用"，阈值靠 prompt 措辞，真机验收阶段观察是否过松（好图被标 unusable）或过严（明显废片没标）。判据措辞可调，不承诺一次到位。
- **pairwise 单次延迟 / 两张图上传成本**：一次发两张 downscale 到 ≤1024px 的图，本地视觉模型的单次比较延迟没有实测基准；本阶段只验证正确性，量级/进度问题留目标二锦标赛真机基准时评估。
- **`assessment` 是自由文字、非结构化**：跨图检索、按维度过滤这类需求本阶段不支持（PRD 明确要"整体评价"）；若以后要结构化，是另一次独立改形，不在此预留。
- **`request_json` 多图重载对 provider 请求体的兼容**：Claude/Gemini 多 image 元素的顺序与 text 位置按现有单图布局顺延（图在前、text 在后），真机验收确认多图请求被三个 provider 正确接收、不因数组结构变化报 4xx。
- **废片以外的次品不再被硬过滤**（2026-07-21 补充）：eval 不整批跑之后，curate/dedup 只靠废片标签排除，其余质量筛选全部交给锦标赛（目标二）。"次品会在锦标赛里被淘汰"这个假设未经真机验证——若锦标赛最终选出明显次品，需回看是不是要补一层轻量过滤，或让部分用户仍能在 agent 流程里 opt-in 一次 eval。本阶段不为这个尚未出现的问题预留机制。
- **dedup 新增排废片的边界**：废片与好图互为近邻时排废片是对的；但若一簇里全是废片（用户把一组连拍都手动标了废片），排除后该簇消失、无重复标记，符合预期（都不想要）。真机确认排除逻辑不误伤"废片标签恰好来自别的语义"的场景（沿用 `ensure_reject_tag` 既有的"标签名冲突交给用户保证"惯例）。
