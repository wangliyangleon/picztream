# PicZTream (PZT) 工程设计文档（W2026-07-21 目标二：dedup 两类 + curate 两模式 + 全局 AI 开关）

## 背景

本文档是 `docs/W2026-07-21_PRD.md` 两阶段方案的第二份 Eng Design，覆盖目标二（接线）。目标一（地基）已完成并归档为 `docs/W2026-07-21_Eval_Eng_Design.md`：eval 跟比较解耦、dedup/curate 降级到"非 AI 基线"、建好 pairwise 视觉比较能力（`core/ai/compare.h` + `pzt compare`）。

目标二要把 AI 锦标赛接上 dedup/curate，并用一个全局开关控制走不走 AI。

**对 PRD 原文的一处修订**：PRD"已拍板的关键决策"第 2 条原文是"pairwise 视觉比较...落在 `core/ai`...**bracket 推进（谁跟谁比、如何晋级）落在 agent（Python）**"。规划阶段跟用户对齐后改为：**整个锦标赛（分簇 + 场次推进 + 判定胜者）都收进 core，一次 headless 调用做完**。理由：

- bracket 推进本身是纯确定性算法（给定一批候选 + 一个"谁赢"的比较函数，怎么两两配对晋级），不含任何需要 agent 层判断的业务逻辑，没有理由跨进程往返。
- 收进 core 后，dedup 的 AI 模式和 curate 的 AI 模式共用同一套"分簇 → 选出每簇 winner"实现，不需要在 C++ 和 Python 两边分别维护一份锦标赛算法、承担两份行为可能漂移的风险。
- 避免了 agent 侧原本要处理的两个麻烦：(a) 一个簇内 N 张图要发 N-1 次 `pzt compare` 子进程调用，中途取消/续跑的粒度很难看；(b) 收进 core 后是单次阻塞子进程调用，跟今天 `pzt dedup`/`pzt curate` 已有的取消机制（`agent/session/worker.py` 的 `KILLABLE_STAGES`，整进程 kill）完全对齐，不需要新机制。
- `pzt compare`（单次两图比较的 headless 命令，目标一已建好）不受影响，继续作为独立的人工/脚本工具存在，只是不再是锦标赛内部实现的必经之路——锦标赛内部直接调 C++ 函数 `ai::request_comparison`，不经子进程往返。

## 已对齐的核心设计

1. **废片永远不进分簇/锦标赛**（dedup 和 curate 都排除）；**curate 额外排除"重复"标签**，dedup 不排除。排除规则统一走一个 `exclude_tag_names` 参数，不硬编码"废片"/"重复"两个特判，为将来留口子。

   curate 排除"重复"的理由：dedup 在 agent 流程里是**可选**的一步。如果用户先跑了 dedup 再跑 curate，curate 不该把已经在 dedup 锦标赛里"打输了"的近似重复图再捞回来重赛一次；如果用户压根没跑 dedup，curate 自己的粗粒度分簇本来就会把这些近似重复的图聚到同一簇，选簇内 winner 时已经天然去重过一遍——跳过 dedup 不会漏掉去重效果。这也是 dedup 能够安全地成为可选步骤的原因。

2. **dedup**：细粒度分簇（挑出"近乎同一张"的照片，沿用 `Settings.dedup_time_window_seconds`/`dedup_hash_threshold`，默认 10s/5）。AI 开：锦标赛选 winner，其余打"重复"；AI 关：选 `captured_at` 最新的（今天的行为，不变）。

3. **curate**：粗粒度分簇（挑出"同一场景"，簇间要有差异，沿用 `Settings.curate_time_window_seconds`/`curate_hash_threshold`，默认 20s/10）。每簇选 1 个 winner：AI 开走锦标赛，AI 关直接选 `captured_at` 最新——**这一条统一成同一个函数的参数分支，不是另一套代码**；winner 不打"重复"标签（跟"精选"这类目的地标签是两个维度，不该混）。簇的 winner 集合确定后，还有一层 dedup 没有、curate 独有的"从 winner 里挑最终 N 张"：AI 开 = 随机挑 N（PRD 已拍板接受不可复现）；AI 关 = 沿用今天 `greedy_pick` 的 `captured_at` farthest-point 多样性挑 N。

4. **全局 AI 开关**：一个开关同时控制 dedup 和 curate 走不走 AI（PRD 原文"一个统一开关"），不做成两个独立开关。**eval 不受这个开关门控**——它已经在目标一收紧为 pzt CLI 手动功能，agent 完全不碰。

## 现有代码基础（复用，不重造）

- **`core/dedup/dedup.h`/`.cpp` 的 `find_duplicates`**（`dedup.h:71-74`）：纯聚类原语，参数化 `(image_ids, time_window_seconds, hash_threshold)`，返回 `std::vector<DuplicateGroup>{image_ids, keep_id}`。dedup 和 curate 的粗/细分簇都复用它，不重新实现 dHash/时间窗/并查集。**它只返回 size≥2 的组**，未落进任何组的候选（单例）需要调用方自己从候选集里减去分组结果来求得。
- **`dedup::pick_keep_id`**（`dedup.cpp:104-121`）：非 AI 模式选 winner 的规则——`captured_at` 最新，时间相等则 id 最小。
- **`core/ai/compare.h`/`.cpp` 的 `request_comparison`**（目标一已建好）：两图一次会话比较，必须二选一（禁止平局），返回 `{winner: 0|1, reasoning}`。锦标赛内部直接调这个 C++ 函数，不经 `pzt compare` 子进程。
- **`core/tagging` 的 `find_tag_by_name`/`images_with_tag`/`ensure_duplicate_tag`/`add_tag`**：标签排除与打标的现成积木，dedup.cpp 和 curate.cpp 今天各自内联一份（分别排废片、排废片+重复），本次收口成一份共用实现。
- **`core/curate/curate.cpp` 的 `greedy_pick`**（`curate.cpp:90-127`）：`captured_at` farthest-point 多样性挑 N，AI 关时的"最终选 N"继续用它，输入源从今天的 `build_cluster_reps` 换成新原语返回的 winner 集合。
- **`cli/commands/commands.cpp` 的 `cmd_compare`**（`commands.cpp:530-590`）：`--provider` 解析、`decode_image_for_ai`、错误码稳定串的现成写法，`cmd_dedup`/`cmd_curate` 新增的 `--ai`/`--provider` 直接复用。
- **`agent/stages/style.py`** + `orchestrator/driver.py` 的 `rerun_stage`/`rearm_gate`/`_downstream_of`：对话式调整"只重跑受影响的下游、不重跑上游"的既有机制，锦标赛结果不满意时的重跑走同一套，不需要新机制。

## 决策一：核心新模块 `core/tournament/tournament.{h,cpp}`

新模块，同时依赖 `core/dedup`（复用 `find_duplicates`）和 `core/ai`（`request_comparison`）。这两个依赖方向都不新鲜——`curate.cpp` 今天已经依赖 `dedup.h`，新模块是"在两者之上再组合一层"，分层干净。刻意不让 `dedup.cpp` 反过来依赖 `core/ai`：保留目标一"dedup 聚类算法跟 AI 解耦"的精神，聚类原语本身继续零 AI 依赖，只是多了一个带 AI 参数的可选调用方。

```cpp
namespace pzt::core::tournament {

struct ClusterChoice {
  std::vector<project::ImageId> members;  // 簇内全部成员（含 winner）；size==1 是未聚类的单例
  project::ImageId winner;
};

struct ChooseSummary {
  std::vector<ClusterChoice> clusters;   // 含单例"簇"，保证调用方拿到完整候选池，不只是聚类命中的部分
  int tagged_count;                       // apply_dup_tag=true 时被打上"重复"的图片数；否则恒 0
  int skipped_no_capture_time;
  int ai_fallback_count;                  // 见下面"AI 调用失败怎么办"
};

Result<ChooseSummary, project::ProjectNotFoundError> cluster_and_choose(
    db::Database& db, project::ProjectId project_id,
    const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold,
    const std::vector<std::string>& exclude_tag_names,
    bool apply_dup_tag,
    bool ai_enabled, ai::Provider ai_provider = ai::Provider::Local,
    const ai::LocalModelConfig& local_config = {});

}  // namespace pzt::core::tournament
```

### 内部流程

1. 用 `exclude_tag_names` 逐个 `tagging::find_tag_by_name` + `tagging::images_with_tag` 求并集，从 `image_ids` 里摘掉（泛化今天 dedup.cpp/curate.cpp 里各自硬编码的单标签排除）。
2. 调 `dedup::find_duplicates`（细/粗参数由调用方传，聚类算法本身不感知 AI 开关）得到 `DuplicateGroup` 列表；未落进任何组的候选单独收集成"单例簇"，跟分组结果合并，保证 `clusters` 覆盖全部候选——curate 挑最终 N 张需要完整候选池，不能只看聚类命中的部分。
3. 每个簇选 winner：
   - `ai_enabled=false`：复用 `dedup::pick_keep_id` 同款规则。单例簇（1 成员）恒定该成员是 winner，不需要走这条规则。
   - `ai_enabled=true`：簇内单淘汰锦标赛，两两 `ai::request_comparison`。图片解码一次，dHash 计算和 AI 上传共用同一份 `DecodedImage`，不重复解码。奇数轮空位轮空直接晋级。单例簇不发起任何 AI 调用（没有可比对象）。
4. `apply_dup_tag=true` 时：对每个 size≥2 的簇，非 winner 成员打"重复"标签（`ensure_duplicate_tag` + `add_tag`），落库前先在 `image_ids` 全量范围清一遍旧"重复"标记，跟今天 `find_and_tag_duplicates` 的清标签时机一致。
5. 返回 `ChooseSummary`。

### AI 调用失败的处理

某一簇锦标赛中途某次 `request_comparison` 失败（网络错误等），**只对这一个簇**退化成非 AI 的"最新优先"规则选 winner，不中断整个命令、不影响其它簇；`ai_fallback_count` 记下退化了几个簇，通过 headless JSON 输出让 agent/用户看到"有几组因 AI 失败退化成按时间选"，不是完全沉默地降级。这跟 dedup 现有"单张图解码失败只跳过这一张、不拖累其它簇"的容错哲学一致。

## 决策二：`dedup`/`curate` 收口成 `cluster_and_choose` 的两个调用方

- **`dedup::find_and_tag_duplicates`** 签名新增 `bool ai_enabled = false, ai::Provider provider = ai::Provider::Local, LocalModelConfig local_config = {}`（默认值保证现有调用点零改动）。内部改成调 `tournament::cluster_and_choose(..., exclude_tag_names={"废片"}, apply_dup_tag=true, ai_enabled, provider)`，把返回的 `ChooseSummary` 翻译回现有 `DedupSummary{group_count, tagged_count, skipped_no_capture_time}` 形状（外部 API 形状不变，`group_count` 统计 size≥2 的簇）。今天内联在 `find_and_tag_duplicates`/`pick_keep_id` 里的分簇后处理逻辑搬进 `tournament.cpp`，`dedup.cpp` 只留 `find_duplicates`（纯聚类原语，继续零 AI 依赖）和一层薄翻译。

- **`curate::curate`** 签名新增同样三个参数（默认 `ai_enabled=false`）。内部：`resolve_candidates` 的标签排除逻辑收进 `tournament::cluster_and_choose` 的 `exclude_tag_names={"废片","重复"}`（curate.cpp 自己不再单独查标签），拿到 `ChooseSummary.clusters` 后按现有分支挑最终 N：
  - `clusters.size() >= count`：`ai_enabled=false` 时跑现有 `greedy_pick`（farthest-point 多样性，逻辑不变，输入源从 `build_cluster_reps` 换成 `ChooseSummary` 里每簇的 `winner`）；`ai_enabled=true` 时从 winner 集合里随机采样 `count` 个（不做种子化，PRD 已拍板接受不可复现）。
  - `clusters.size() < count`：两种模式都返回全部 winner（`returned < requested`，现有语义不变）。

- **`core/api.h` 门面**（`find_and_tag_duplicates`/`curate_images`）签名同步透传三个新参数，默认值保持向后兼容。

**订正（实现 Commit 2 时发现）**：上面"`DedupSummary`/`CurateResult` 外部形状不变"是疏漏——决策三要在 `cmd_dedup`/`cmd_curate` 的 JSON 里加 `ai_fallback_count` 字段，而这两个命令只调 `find_and_tag_duplicates`/`curate()` 这两个门面，不直接摸 `cluster_and_choose` 的返回值，所以这个数字必须经 `DedupSummary`/`CurateResult` 带出来。实际改动：两个结构体各新增一个 `int ai_fallback_count` 字段（`ai_enabled=false` 时恒为 0）。另外 `find_and_tag_duplicates` 原有的 `on_progress` 回调也一并补进了 `cluster_and_choose`/`cluster_and_choose_impl`（追加在参数列表末尾、带默认值，不影响已有调用点），避免委托过去之后这个参数被静默吞掉。

## 决策三：headless 命令面

`cmd_dedup`/`cmd_curate`（`commands.cpp`）各自新增两个可选 flag：`--ai`、`--provider <local|gemini|claude>`（`--provider` 只在 `--ai` 出现时有意义，复用 `cmd_compare`/`cmd_eval` 已有的 provider 解析代码）。**不出现 `--ai` 时两个命令的调用路径、参数、JSON 输出逐字节不变**——这是向后兼容的硬要求，回归测试要覆盖。

`apply_dup_tag`/`exclude_tag_names` 不升级成 CLI flag——它们是 dedup/curate 两个调用方各自的固定策略（dedup 恒 `apply_dup_tag=true`/排废片；curate 恒 `apply_dup_tag=false`/排废片+重复），不是用户需要在命令行选择的东西。

JSON 输出：`cmd_dedup` 现有输出（`groups`/`tagged`/`skipped_no_capture_time`）不变，`--ai` 模式下补一个 `ai_fallback_count` 字段。`cmd_curate` 现有输出（`requested`/`returned`/`selected`）不变，同样补 `ai_fallback_count`。

## 决策四：agent 侧改动

因为锦标赛整个收进 core，agent 侧不需要新的 bracket 模块、不需要给 `pzt_client` 加新方法、不需要处理"一个 stage 发 N 次 compare 子进程调用"的取消粒度问题——`DedupStage`/`CurateStage` 的 `run()` 只是在已有的 `client.call("dedup"/"curate", ...)` 参数列表后面，按 `params` 里的开关多拼两个 flag。

### 删除 `agent/stages/evaluate.py` 及全部引用

- `agent/stages/dedup.py`：`inputs` 从 `["Evaluate"]` 改 `["Ingest"]`。
- `agent/compose/plan_composer.py`：删 `Evaluate` 的 `StageSpec`；`provider`（LLM 已经在推断的字段）改成挂到 `Dedup`/`Curate`（而不是消失的 `Evaluate`）和继续留在 `Style` 上。
- `agent/compose/validate.py`：`_EXPECTED_STAGE_NAMES` 删 `"Evaluate"`；删 `bad_evaluate_provider`/`bad_evaluate_auto_reject` 两段校验（`auto_reject` 概念随 Evaluate 一起消失——agent 侧不再有"自动剔除不合格"这件事，不合格判定只来自人工在 `pzt open`/`pzt eval` 手动跑出来的废片标签）；新增 `ai_enabled`（bool）+ `ai_provider`（枚举同 `_VALID_PROVIDERS`）两段校验，挂在 `Dedup`/`Curate` 上。
- `agent/run_intent.py`/`run_watchfolder.py`/`run_telegram.py`：去掉 Evaluate 的 import + 注册；`run_watchfolder.py` 的 `--provider`/`--no-auto-reject` CLI flag 视情况改指向新开关或删除。
- `agent/session/worker.py`：`KILLABLE_STAGES` 去掉 `"Evaluate"`（`"Dedup"` 保留；`"Curate"` 现在因为可能带锦标赛，耗时不再恒定 O(1)，一并加入 `KILLABLE_STAGES`）。
- `agent/session/view.py`：删 Evaluate 的进度文案；`plan_summary` 改成从 `Dedup`/`Curate` 的 params 读 `ai_enabled`（而不是从 Evaluate 读 `provider`/`auto_reject`）。
- `agent/session/consumer.py`：`_check_eval_poll` 整个方法删除（没有 eval 阶段可轮询了）；`_current_plan_params`/`_send_plan_confirmation` 重写——不再提"自动剔除不合格"，改成视 `ai_enabled` 决定文案（例如"AI 帮你从相似照片里挑更好的" vs "按拍摄时间挑"）；`_on_refine_reply` 从写 `evaluate.params` 改成写 `dedup`/`curate` 的 `ai_enabled`/`provider`。

### 全局 AI 开关的落点

`ai_enabled`（bool）+ `provider`（string）两个字段，**同时**写进 `Dedup` 和 `Curate` 两个 `StageSpec.params`（复制，不新增 `Plan` 级字段）。理由：`Plan` 目前没有任何跨 stage 字段先例（`orchestrator/types.py` 的 `Plan` 只有 `stages: list[StageSpec]`），新增一个要连带改 `run_state_from_dict` 的手工反序列化和 `StageContext` 的接口，超出这周范围；`provider` 字段今天已经是"复制到 Evaluate 和 Style 两处"的先例，`ai_enabled` 跟着同一个模式最省事。`compose_plan` 的 LLM schema 新增 `"ai_enabled"`（boolean，用户明确要"挑最好的"/"AI 帮我选"才给 true，含糊不清默认 false——对齐 PRD 背景"opt-in 重路径，不默认全量"）。

`DedupStage`/`CurateStage` 改动：`params.get("ai_enabled", False)` 为真时，在现有 argv 后追加 `--ai --provider <provider>`；为假时 argv 不变（今天的调用逐字节保留）。

## 决策五：测试

- `core/tests/tournament_test.cpp`（新）：`cluster_and_choose` 单测——AI 关时选 winner 等价于 `pick_keep_id` 规则；AI 开时用可注入的 fake `request_comparison`（照抄 `compare_test.cpp` 的 `HttpPostFn` 注入模式）验证锦标赛推进/晋级/单例簇不发起 AI 调用；某次比较失败时该簇退化、`ai_fallback_count` 正确；`exclude_tag_names` 排除多个标签；`apply_dup_tag` 开关控制是否落"重复"标签。
- `core/tests/dedup_test.cpp`：新增 `--ai` 路径的集成用例（走 `find_and_tag_duplicates(ai_enabled=true, ...)`），非 AI 路径现有用例保持不动（回归）。
- `core/tests/curate_test.cpp`：新增 AI 模式用例（簇 winner 从锦标赛来、最终 N 张来自随机采样——固定 fake 比较结果验证"确定性输入下选出确定性 winner 集合"，随机采样这一步只验证"结果是 winner 集合的子集、大小正确"，不验证具体顺序）；非 AI 路径现有用例保持不动。
- `cmd_dedup`/`cmd_curate` 的 headless 测试：`--ai`/`--provider` flag 解析、不带 `--ai` 时现有行为逐字节不变的回归断言。
- `agent/tests/stages/test_dedup.py`/`test_curate.py`：新增 `ai_enabled=True` 时 argv 正确带上 `--ai --provider ...`。
- `agent/tests/stages/test_evaluate.py` 删除；`agent/tests/compose/test_plan_composer.py`、`test_validate.py`、`orchestrator/test_driver_*.py`（用到 `"Evaluate"` 占位名的）、`session/test_consumer_flow.py` 按引用位置更新。

## 任务分解（TDD RED→GREEN，一个可提交单元一 commit）

1. **core 锦标赛原语**：`tournament::cluster_and_choose` + 单测（不接入 dedup/curate，独立可验证）。
2. **dedup/curate 收口**：两个函数签名扩展 + 内部改调 `cluster_and_choose` + 现有测试保持绿（回归）+ 新增 AI 路径测试。
3. **headless 命令面**：`cmd_dedup`/`cmd_curate` 加 `--ai`/`--provider` + 输出补 `ai_fallback_count` + 测试。
4. **agent：删 Evaluate**：整个删除清单一个提交单元（文件删除 + 全部引用点更新 + 测试更新，一次性做完防止中间态跑不过 CI）。
5. **agent：接 AI 开关**：`plan_composer`/`validate` 加 `ai_enabled` 字段 + `DedupStage`/`CurateStage` 透传 + `consumer.py` 文案重写 + 测试。

## 非目标（本阶段不做）

- Apple Vision 聚类（`docs/Task_Pool.md` 单列）。
- 锦标赛以外的比较拓扑（瑞士轮、全量两两评分排序）——本周只做单淘汰锦标赛。
- AI curate 随机选 N 的可复现性（seed 化）——先接受随机。
- 把 Dedup 做成 Plan 里可整段跳过的可选 stage——本阶段"废片/重复标签排除"已经让跳过 dedup 变得安全（见"已对齐的核心设计"第 1 条），但 Plan 固定 6 段 stage 列表本身不改，Dedup 仍然总会跑（只是 AI 关时它是一个廉价的确定性步骤）。真要做"用户显式跳过 dedup"的对话入口，留到之后按需再立项。
- 锦标赛进度播报（复用 consumer 轮询进度的先例）——真机跑出实际延迟数据后再决定要不要加。

## 验证

- `core/tests` 全绿，`agent` 测试套件全绿。
- 同时构建 Debug（`build/`）与 Release（`build_release/`）。
- 真机：`pzt dedup <proj> --scope '*'`（不带 `--ai`）行为、输出跟今天逐字节一致；`pzt dedup <proj> --scope '*' --ai --provider local` 能跑通，"重复"标签落在锦标赛输的一方；`pzt curate <proj> --count N`（不带 `--ai`）行为不变；`--ai` 模式下每簇锦标赛出 winner、最终随机挑 N 张；agent 一次 Run 端到端：AI 关全程零模型调用、AI 开正确触发 dedup/curate 两处的锦标赛。
- 全局开关关时的 Run 不产生任何 AI 调用（PRD 验收标准原文）；开时 pairwise 调用次数符合"k 张簇 k-1 次比较"的量级（真机粗测一次即可，不需要精确基准）。

# 目标三补充设计（2026-07-23）：dedup/选片流程可选化

## 背景

`docs/W2026-07-21_PRD.md` 目标三已拍板：Dedup/Curate 两步各自要不要跑由这次意图判断（三分支：没提去重→直接 Curate；提去重没数量→先 Dedup 再追问；提去重带数量→跳过独立 Dedup 直接 Curate(N)），外加"没提 AI 就提醒+给快捷按钮"。手动选片已移出范围，留给未来单独立项。上面"非目标"一节曾把"Dedup 做成可跳过的 stage"列为延后项，这一节就是把它做完。

规划阶段通读了 orchestrator（`agent/orchestrator/types.py`/`driver.py`）、`agent/stages/curate.py`/`deliver.py`、`agent/compose/plan_composer.py`/`validate.py`、`agent/session/consumer.py`/`worker.py`，以及本文档决策一二涉及的 core `curate.cpp`，过程中发现两个原计划之外、必须处理的架构缺口（决策二、决策三），已经想清楚最小修法。

## 决策一：compose_plan 的两个新字段与三分支落 Plan 形状

`_SCHEMA_INSTRUCTION` 新增两个字段：

- `dedup_requested`（boolean）：用户是否明确提到"去重"。
- `count` 从"必填 int，默认 9"改成**可空**：用户明确给了目标张数才填数字，纯"去重"意图不该被默认值 9 污染。

`compose_plan` 按下面规则决定 Plan 形状（代码里判断，不是 LLM 判断）：

- `count is not None`（不管 `dedup_requested` 是什么）：**不生成 Dedup StageSpec**，直接 `Curate(count=count)`。对应 PRD 案例一（没提去重）和案例三（提去重+给了数量）——两者收敛成同一个 Plan 形状，因为 core `curate` 的粗聚类已经隐含去重效果（决策二引用的 `cluster_and_choose`，见本文档"已对齐的核心设计"第 1 条），单独跑一次 Dedup 是多余的。
- `count is None and dedup_requested`：生成 `Dedup(...)` + `Curate(count=None, ..., gate="required")`——对应案例二，Curate 的决定推迟到 Dedup 跑完之后用一个新闸门问。
- `count is None and not dedup_requested`：`count` 按现在的规则 fallback 成默认 9，走跟第一条一样的"不生成 Dedup"路径——等价今天的行为。

`ai_enabled`/`provider` 仍然是全局一份，写进 Plan 里实际存在的每个 stage（Dedup 存在就写，Curate 恒写）。

## 决策二（架构缺口，必须处理）：`CurateStage.inputs` 的依赖声明要跟着改

`agent/stages/curate.py:15` 现在硬编码 `inputs=["Dedup"]`。`Driver._next_pending`（`agent/orchestrator/driver.py:141-156`）拿 `stage.inputs` 里每个名字去 `run.stage_states` 查，查不到就当"未解决的上游"，永远返回非空 `unmet`——一旦 Dedup 不在这次 Plan 里（决策一第一分支），会对 Curate 抛 `RuntimeError("Plan is not topologically ordered")`，Curate 永远跑不起来。

`CurateStage.run()` 本身从不读 `ctx.outputs["Dedup"]`（它直接调 `pzt curate` 现查现选），`inputs=["Dedup"]` 纯粹是历史遗留的顺序声明，不是真实数据依赖。改成 `inputs=["Ingest"]`：Plan 里 Dedup 存在时，list 顺序（Ingest→Dedup→Curate）本身已经保证先后，`inputs` 不需要再重复声明；Dedup 不存在时，Curate 直接排在 Ingest 后面，检查通过。`_downstream_of`（级联重置用）目前没有任何"调整 Dedup 参数后要级联重置 Curate"的场景（Dedup 参数只在跑之前改，从没有"跑完之后回头改 Dedup 重跑"的调整路径），去掉这条声明不影响现有级联行为。

## 决策三（架构缺口，必须处理）：Curate 的"passthrough 模式"——不跑聚类，原样交付 Dedup 幸存者

案例二用户说"够了，不用再筛了"时，不能直接跳过 Curate stage 不跑：`DeliverStage.run()`（`agent/stages/deliver.py:82,90`）硬编码只读 `ctx.outputs.get("Curate")` 拿 `selected` 列表，Curate 缺席就是空列表，什么都不会交付。也不能简单地"照样调 `pzt curate --count M`"（M=去重后剩余张数）冒充"不筛选"：core `curate()`（`core/curate/curate.cpp:110-152`）的语义是"每个粗聚类簇最多出 1 个 winner，`count` 只是给簇数封顶"，不是"从候选里选 top count 个"——如果 M 个去重幸存者里还有几个被粗聚类分进同一簇，`--count M` 一样会把它们收窄成 1 个，等于变相又做了一次用户明确拒绝的"筛选"，语义不对。

方案：`CurateStage.run()` 新增分支——`params["count"] is None` 时不调 `pzt curate`，改调 `pzt images <project> --json`，客户端过滤掉 `tags` 含"重复"或"废片"的图，拿到的 path 列表就是"直接交付"的最终 `selected`，然后走跟现有分支一样的 `tag clear` + `tag apply` 收尾。**这条路径纯粹是 agent 侧新增的 Python 分支，不需要改 core/CLI**（`pzt images --json` 已经把每张图的 `tags` 数组吐出来了，见 `cli/commands/commands.cpp:653-698`）。`ai_enabled`/`provider` 在这个分支下无意义（没有比较，不存在 AI 不 AI），直接忽略，不报错。

## 决策四：Dedup 完成后的追问，复用现有闸门机制（不新增 Driver 能力）

`Curate` 在决策一第二分支里带 `gate="required"`——这不是新的 Driver 概念，`compose_plan` 已经在给 `Style`/`StyleApplyAll` 这么干（决策四原文，`agent/compose/plan_composer.py:59-60`）。Driver 走到 Curate 前会照常停在 `AWAITING_GATE`（`driver.py:37-41`），`SessionWorker._report_stop` 照常发 `GateReached(stage="Curate", payload=...)`。

具体要接的四处：

1. **`worker.py::_prepare_gate_payload`**（`session/worker.py:259-291`，现在是 Style/StyleApplyAll/默认=Deliver 三分支）新增 `if stage == "Curate":` 分支：`remaining = ingest_output.data["image_count"] - dedup_output.data.get("tagged", 0)`，payload `{"remaining": remaining}`。
2. **新分类器** `compose/adjustment_parser.py::classify_dedup_followup(text, remaining)`：新的 schema，输出 `{"action": "narrow"|"skip"|"query"|"cancel", "count": <int|null>}`——跟 `classify_gate_reply`（服务的是"调整已选中结果"的完全不同问题域）分开，不复用。
3. **`consumer.py`**：
   - `_process_text` 的 `AWAITING_GATE` 分派里加 `gate_stage == "Curate"` 一支，走新 classify kind `"dedup_followup"`（context 带 `remaining`，从 `self.view` 或 `self.run.outputs` 取）。
   - `_on_gate_reached` 加 `elif event.stage == "Curate":` 渲染分支：`"去重后还剩 {remaining} 张，要不要再筛选一下留多少张？"`，按钮见决策五。
   - 新增 `_on_dedup_followup_reply(reply)`：`action=="narrow"` → 投 `DriveJob(action="rerun_curate", args={"params": {"count": reply.count}})`；`action=="skip"` → 同样投 `rerun_curate`，`count=None`（触发决策三的 passthrough 分支）；`query`→回 `self.view.describe()`；`cancel`→走 `_prompt_cancel_confirmation()`（跟其它闸门一致）。

   **订正（实现 Commit 8 时发现）**：原文这里写的是复用 `Driver.apply_adjustment` + `resolve_gate`。实现阶段追查 `apply_adjustment`（`driver.py:126-137`）发现它只改 `spec.params`，不改 `spec.gate`——Curate 这时候 `gate="required"` 还挂着，`apply_adjustment` 把 `run.status` 设回 `RUNNING` 后，`advance()` 会重新走到 Curate、发现它的 `gate != "off"` 且 `gate_state is None`，又把同一个追问闸门重新挂起一次，用户刚回答"留5张"会被原样再问一遍，死循环。改用 `driver.rerun_stage`（`driver.py:104-124`）：它的设计初衷就是"闸门已经问过、这次给的就是答案，不需要闸门再问一遍"（Style 描述闸门是这么用的），直接调 `_run_stage` 跑掉 Curate，完全绕开 `advance()` 的闸门检查，不需要碰 `.gate` 字段，也**不新增 Driver 方法**（`rerun_stage` 已经是现成的）。`worker.py::_execute_drive` 对应新增 `elif job.action == "rerun_curate":` 分支，同 `rerun_style` 的写法，额外因为 Curate 在 `KILLABLE_STAGES` 里而手动布防/解防 `cancel_event`（绕开了 `_drive_to_stop` 循环自带的布防）。

## 决策五：AI 可发现性——提醒文案 + 快捷按钮

两个触发点，各自只在"这一步的 `ai_enabled` 为 False"时出现：

- **PLANNED 确认**（`_send_plan_confirmation`）：决策一第一分支（Curate 直接确定）沿用目标二已有的按钮结构，在原 `_CONFIRM_BUTTONS`（`[("好的 ✅", approve)]`）基础上追加 `("AI筛选 🤖", ai_curate)`；决策一第二分支（Dedup 先跑、Curate 待定）确认文案换成"先帮你去重，去重完再问要不要接着筛"，按钮追加 `("AI去重 🤖", ai_dedup)`。
- **决策四的追问闸门**：按钮在"不筛选了"之外追加 `("AI筛选 🤖", ai_narrow)`——点了视为"narrow，count=remaining，ai_enabled=True"（不问具体数字，AI 从"要不要更狠地筛"这个模糊意图里直接按 remaining 张的默认策略跑，跟"打字给数字"路径共存，不互斥）。
- 新按钮 token 的点击处理并入 `_handle_callback`（`consumer.py:316-358`）：新增 `_BTN_AI_DEDUP`/`_BTN_AI_CURATE`/`_BTN_AI_NARROW` 三个 token，效果分别等价于"文字回复里带了 AI 意图"，直接调用对应状态的既有处理函数（`_on_refine_reply`/`_on_dedup_followup_reply` 的"confirmed/narrow"分支），不需要真的过一遍 LLM 分类。

**补充（实现 Commit 8 时发现）**：`_send_plan_confirmation`/`view.py::describe()` 在 deferred 形状下（`count is None`）如果不分支会直接把 `None` 插进"留 None 张"这种文案，这是 Commit 8 就得处理的正确性问题，不能拖到这里——Commit 8 已经把"先帮你去重，去重完再问要不要接着筛"这句不提 AI 的版本接上了，这里（Commit 9）要做的只是在这句话基础上加 AI 提醒 + 上面列的按钮，不是从零建这个分支。

## 决策六：`validate.py` 适配两种 Plan 形状 + `count` 可空规则

`_EXPECTED_STAGE_NAMES`（单一 list、`!=` 精确比较）改成两个合法形状的集合：`_WITH_DEDUP` / `_WITHOUT_DEDUP`，`names` 必须精确匹配其中一个，不做宽松的"contains"检查（保住这道护栏"挡 LLM 输出污染"的本意）。

`count` 校验：`Curate.gate == "required"` 时允许 `count is None`；否则维持原有 `1 <= count <= 50` 的 int 校验。这条规则把"count 为空"和"Curate 被挂起等追问"绑定成同一个不变量，不是两个独立开关，避免出现"count 为空但 Curate 没挂闸门"这种验证漏过的非法状态。

## 决策七：PLANNED 态 refine 提前给数量 = 提前回答追问

决策一第二分支的 PLANNED 确认阶段，用户可能不等 Dedup 跑完就直接在确认时说"留5张"——`_on_refine_reply`（`consumer.py:697-725`）现在拿到 `reply.count` 就直接写 `curate.params["count"]`，但这个分支下 Curate 当前是 `gate="required"` 状态，不能只改 `count` 不改 `gate`（会变成"count 有值但仍然会在 Dedup 后被问一遍"的冗余追问）。

处理：`_on_refine_reply` 的 confirmed 分支在 `curate.params["count"] = reply.count` 之后，`reply.count is not None` 时顺手 `curate.gate = "off"`。

**订正（实现 Commit 8 时发现，跟决策四的订正是同一个原因）**：原文设想的 `_resolve_curate_count` 共享 helper 最终没有必要——决策四改用 `rerun_stage` 之后，`_on_dedup_followup_reply` 的 narrow/skip 分支根本不碰 `.gate`（`rerun_stage` 直接绕开闸门检查），需要清 `.gate` 的只有这里（PLANNED 阶段，Driver 还没开始跑，直接改 Plan 对象本身不会触发决策四提到的"运行中重新挂起"那个坑）。两处场景其实不对称，硬抽一个共享 helper 反而增加一层不必要的间接，直接在 `_on_refine_reply` 里内联这一行更清楚。

## 非目标（本阶段不做）

- 手动选片（用户直接报编号）——PRD 已明确移出，留给未来单独立项。
- Apple Vision 聚类、锦标赛以外的比较拓扑——同目标二。

## 任务分解（建议的 Commit 顺序）

1. **Commit 6**：决策一 + 决策二 + 决策六（`plan_composer.py` 两分支 + `curate.py` inputs 修正 + `validate.py` 两形状/可空 count）——先把"Plan 形状可以正确变化"这个地基打完，配套测试覆盖三种分支产出的 Plan 结构、以及"Dedup 缺席时 Curate 能正常推进"的 Driver 回归测试。
2. **Commit 7**：决策三（`CurateStage` passthrough 分支）——独立可测（给定一批带"重复"标签的 fixture，验证 passthrough 输出排除它们），不依赖 Commit 8 的会话层改动。
3. **Commit 8**：决策四 + 决策七（`worker.py` payload + `rerun_curate` DriveJob action、新分类器、`consumer.py` 新增追问闸门渲染/处理、`_on_refine_reply` 的 `.gate` 清除 + Dedup 缺席防崩）——这是最重的一块，需要新的 `session_fakes.py` fixture 覆盖"Dedup 完成后停在 Curate 闸门"这个新状态。
4. **Commit 9**：决策五（两处 AI 提醒文案 + 三个新按钮 token）——纯文案/按钮层，放最后因为依赖 Commit 8 已经把两个新确认点的骨架搭好。

## 验证（目标三部分）

- 每个 Commit 后 `agent/.venv/bin/python -m pytest -q` 全绿，新增分支要有专门用例（尤其 Commit 6 的"Dedup 缺席时 Driver 不报 RuntimeError"、Commit 7 的"passthrough 排除已打标签图"、Commit 8 的"narrow/skip 两条追问回复都正确落到 Curate params 并解闸门"）。
- Commit 6-9 全部落地后，用跟目标二相同的手法做一次真机/半真机手工验收（`run_intent.py` 不支持闸门交互，这次的追问闸门必须过 Telegram 或新写一个支持 `AWAITING_GATE` 的临时验证脚本，具体验收方式留到实现完成后再定，这里先不假设）。
