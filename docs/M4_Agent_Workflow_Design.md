# PicZTream (PZT) Milestone 4 · Agent 工作流设计（编排骨架）

## 文档定位

这是 M4 **agent 编排骨架的设计规格（design spec）**，2026-07-13 经过一轮结构化 brainstorm 产出。它只回答一件事：**agent 的工作流（编排）长什么样**——有哪些抽象、状态怎么流转、人在哪几处介入、失败/离线怎么恢复、对话怎么驱动状态、怎么测。

**范围边界（重要）**：本 spec **只管工作流**。M4 的其它维度——**本地模型分层设计（Tier 0/1/2）、用例探索、"挑 N 张"的策展算法、与 M3 遗留的交叉、尚未拍板的产品岔路（"照片不出机"硬约束、风格是否含几何变换、其它 IM 接入）**——留在 `docs/M4_Brainstorm.md`，本文不重复。后续的 M4 PRD 会把两份综合成"以功能和 actionable item 为主"的需求文档。

本 spec 是设计骨架，不是最终 Eng Design；具体 schema、模块划分、命令签名留给对应 Eng Design。

## 一、已拍板前提（本设计的地基）

这轮讨论逐条拍板、贯穿全文，后面任何细节不能违背：

- **P1 · 骨架优先**：先设计通用编排骨架（本 spec），再逐步实例化——先 v1 最小闭环（纯粗筛交付：收图 → 送评 → 去重 → 策展 → 交付），再扩到完整朋友圈端到端（+ 风格 + 文案 + 对话式微调）。
- **P2 · 人在环 = 自动跑到底 + 可选闸门**：agent 默认自主跑完整条链，只在少数"花钱 / 不可逆 / 主观审美"的步骤前设**可选**闸门，不回应就按默认继续。闸门全关 = 全自动，全设必答 = 步步盯。末端永远有一次统一确认（"人保留最终拍板权"）。
- **P3 · 触发 = 意图消息驱动**：意图消息（"选 9 张发朋友圈暖色风"）天然兼做"输入齐了 + 这是要求 + 开跑"；辅以显式"开始"兜底；静默计时器在"无意图"时主动问询、在"意图已知"时自动开跑。
- **P4 · 记忆 = 记住上次配置**：Run 独立执行，但每用户保留"上一次 Run 配置"作为下次默认/可引用（"老规矩"），不做完整长期对话记忆。
- **P5 · 实现形态 = 独立 Python 编排进程 + headless CLI 边界（形态 B）**：agent 是 Python 独立进程，通过一组 headless `pzt` 子命令（JSON 进出）驱动 core，不直接链接 core。
- **P6 · "更激进"作为 Stage 参数**：agent 可以比人工路径更激进（如评估不达标直接打废片），但这**只体现为 Stage 的策略参数默认值不同**，绝不去改人工路径的全局 Settings，也绝不让 core 分支感知"谁在调"——物理隔离。

## 二、核心抽象与数据模型

三层抽象，从小到大：

**Stage（步骤类型）** —— 一个可复用的独立处理步骤，只通过明确的输入/输出跟别的 Stage 交互，不共享隐藏状态。每个 Stage 声明：
- `inputs`：依赖哪些前置产物（`Curate` 依赖 `Evaluate` + `Dedup` 的结果）。
- `outputs`：产出什么（`Curate` 产出选中的 image_id 列表）。
- `is_gated`：这步前要不要可选闸门。
- `cost_class`：本地/免费 vs 云端/花钱——决定默认要不要挂闸门。
- `criticality`：关键（失败则整个 Run 无意义，如 `Ingest`）vs 非关键（失败可降级，如 `Style`/`Caption`）。
- **策略参数**：一组"激进度"旋钮（auto-reject-on-fail、auto-exclude-dup、gate 阈值、curate 果断程度……），agent 侧默认比人工更激进（P6）。

初始 Stage 库：`Ingest`（收图建 project）、`Evaluate`（送评）、`Dedup`（去重）、`Curate`（策展挑 N 张）、`Style`（统一风格）、`Caption`（写文案）、`Deliver`（导出 + 回传）。**加能力 = 加一个 Stage 类型。**

**Plan（计划）** —— 一个 Run 要执行的**有序 Stage 调用列表**，每个调用带参数（`Curate(count=9, diversity=on)`）+ 每个闸门的三态设置。**纯数据**：可序列化、可持久化、可人读。由 LLM 在开跑前从 Stage 库**组装一次**，之后可对话调整。

**Run（一次任务）** —— 骨架的核心运行时对象：
- `run_id`、关联的 `project_id`（`Ingest` 建的）。
- `plan`。
- `state`：走到哪个 Stage、每个 Stage 的状态（pending/running/done/failed/skipped）、每个 Stage 的产物指针（落在 core 的 DB 或 agent 自己的存储）。
- `gate_state`：哪些闸门在等、超时时间、用户回应。
- `intent_raw`：用户原话留档。
- **全部持久化** → 崩溃/离线后重载 Run 就从上一个完成的 Stage 续跑。

**两个配套概念**：
- **Profile（目的地模板）**：朋友圈/IG/归档等，本质是"默认 Plan 骨架 + 默认参数"，给意图组装当起点。加目的地 = 加 Profile（数据，不是代码）。
- **上次配置（P4）**：每用户存一份"上次的 Plan + 参数"，下次组装当默认/可引用。就一份。

**贯穿边界**：Run / Plan / Stage **全是 agent 层的东西，core 对它们、以及对"激进度策略"双重无感知**。Stage 实现内部调 core（经 headless 命令），但"Run、Plan、闸门、对话、激进度"这些编排/策略概念绝不下渗进 core。

## 三、状态机与执行模型

**Run 生命周期状态**：
`Collecting`（收图中，等触发）→ `Planned`（意图已解析成 Plan）→ `Running` → `AwaitingGate`（卡闸门等用户，带超时）→ `AwaitingReview`（跑到终点等末端确认）→ `Done` / `Failed` / `Cancelled`。

**驱动器（纯确定性循环）**：
1. 取下一个 pending 的 Stage，查它的 `inputs` 依赖是否都 done。
2. 执行前：若 `is_gated` 且策略要求（`cost_class` = 云端/花钱 或主观），转 `AwaitingGate`、发确认、起超时；同意/超时默认 → 继续，否决 → 转调整。
3. 执行：调对应 headless 命令，拿 JSON 结果。
4. 执行后：标 Stage done、产物指针写进 `Run.state`、**整个 `Run.state` 持久化落盘**。

**检查点 & 断点续跑**：
- **每个 Stage 完成后持久化整个 `Run.state`** 是唯一检查点粒度——Stage 是原子单位。崩溃/离线/重启 → 加载所有非终态 Run → 从"下一个 pending Stage"续跑，**贵的步骤（评估）跑过不再跑**。
- **幂等性要求**：Stage 重跑要安全（"执行完但没标 done"之间崩了会重跑一次）。多数 core 操作天然幂等（`add_tag` 幂等、eval 有 `evaluated_image_ids` 跳过已评、dedup 先清后打）；新 Stage 照此设计（curate 确定性可复现、style-apply = `set_image_recipe` 天然幂等）。唯一例外见第八节的 `Deliver`。

**核心原则**：**驱动器本身零 LLM、纯确定性**——只是"取 Stage → 查依赖 → 过闸门 → 调命令 → 存状态"的循环。所有 LLM（意图→Plan、写文案）都发生在**特定 Stage 内部或 Plan 组装阶段**，不在驱动循环里。这是"可测"的根源（第九节）。

## 四、闸门机制

**哪些 Stage 默认带闸门**——由 `cost_class` + 主观性决定：
- `Ingest` / `Evaluate`(本地) / `Dedup` / `Curate` / `Tag` / 导出本地文件 → 本地、便宜、可逆 → **默认无闸门**。
- `Style`（套风格）→ 主观审美 → 默认带闸门。
- `Caption`（云端写文案）→ 花钱 → 默认带闸门。
- 若 `Evaluate` 走**云端** provider（几十张 = 花钱）→ 那一步也带闸门。**闸门挂不挂看解析后的 provider/cost，不只看 Stage 类型。**
- 末端交付不算"闸门"，是第六节的 `AwaitingReview` 终点确认（恒在）。

**闸门默认语义**："说话，否则我继续"——发确认、起超时；同意/超时 → 继续（默认方向 = proceed，因为这些动作本就是用户意图里点名要的，闸门只是"抬头知会"）；提修改 → 转成一次调整（第六节）。

**每个闸门可配置三态**：`off`（从不问）/ `courtesy`（问一声、超时默认继续）/ `required`（必须答、不答就等）。全关 = 全自动，全 `required` = 步步盯（P2 的两极）。`on_timeout` 也是每闸门策略：绝大多数 `proceed`；真不可逆/贵的可设 `hold`。

**闸门 vs 终点确认**：结构相似（都让驱动器暂停等输入），但闸门是中途"要做 X 行吗"、可选可配；终点 `AwaitingReview` 是"成品在此，approve/改/打回"、恒在。

## 五、触发、进料、意图 → Plan 组装

**进料 & 触发**（P3）：照片经传输到达、无活跃 Run → 归到该用户一个 `Collecting` Run。照片和意图**可任意顺序到**，Run 把两者攒着。`Collecting → Planned` 触发：
- **意图消息到** → 解析 → 组装 Plan → 开跑（主路径）。
- **显式"开始/收工"** → 有意图就跑，没意图先问（兜底）。
- **静默计时器**——动作取决于意图是否已知：有图无意图 → 到点**主动问**"看到你发了 N 张，想怎么处理？"；有图且意图已知 → 到点**直接开跑**。

**意图 → Plan 组装**（那"一次性"的 LLM 步）：
- 输入：意图原文 + 推断的 Profile + 上次配置（P4）+ Stage 库。
- 输出：结构化 Plan（有序 Stage + 参数 + 每闸门三态）。走 M3 现成的 `request_json` 模式，schema 约束模型只回 Plan 的 JSON。
- **组装后一道确定性校验**：每个 Stage 存在、参数在范围、依赖可满足——不合法就让模型修一次或退回 Profile 默认并告知用户。**这道校验挡住 LLM 输出污染确定性驱动器。**
- 之后驱动器纯确定性执行——即"plan-once-then-execute"。

**一点透明度（非闸门）**：开跑回一句 plan 摘要（"好，40 张里帮你选 9 张、套暖胶片、写朋友圈文案，开跑~"），只知会、不阻塞。

## 六、对话绑定与调整模型

**会话绑定 & 消息归类——状态优先、内容其次**：一条会话绑一个用户的当前 Run，**Run 状态先给强先验**，再看内容细分：
- `AwaitingGate`（有待答问题）→ 大概率闸门回答：肯定 → 继续；明显改动 → 当调整；明显无关 → 单独处理。
- `AwaitingReview` → approve / 调整 / 打回。
- `Running`（无待答闸门）→ 插话 → 捕获成待应用调整、在下一个 Stage 边界应用，不打断正在跑的 Stage。
- 无活跃 Run / `Done` → 带图/意图 → 新 Run（回第五节）。
- 归类：明显的（y/行/换掉第3张）走廉价规则短路；"是不是改动、改什么"交 LLM——跟调整解析同一个 LLM 调用。

**调整模型（对话驱动状态的核心）**：
- 一次调整由 LLM 解析成**结构化配置增量 + 受影响 Stage 集合**："换掉第3张"→`Curate`(排除、补1张)；"文案活泼点"→`Caption`(tone=lively)；"风格冷一点"→`Style`(调参)；"要10张"→`Curate`(count=10)。
- 驱动器据此**把受影响 Stage 及其下游全部作废（回 pending），只从最早受影响处重跑子图**；上游不受影响的**不重跑**——改文案不重跑 eval/dedup/curate/style；改 curate 重跑 curate→style→caption→deliver 但不碰贵的 eval/dedup。
- **这是确定性 Stage 设计的最大红利**：对话迭代 = "作废受影响 Stage、重跑子图"，便宜、确定、复用已算好的贵结果。
- 同一道确定性护栏：增量只改**已有 Stage 的参数**（有界词汇），走 Plan 组装同款校验；LLM 从不新造控制流。

**P4 老规矩**：Run 完成时把最终 Plan + 参数存成"上次配置"；下次组装当默认，"老规矩"直接照搬。

**终点确认 + 预览/交付两段式**：复核时发**内联照片预览**（快、够瞄）；**approve 后才发最终文件**（字节精确的成片）。即 `Deliver` 天然两段：预览待审 →（批准）→ 文件交付。approve → `Done`（存 last-config）；调整 → 重跑子图 → 回复核；打回 → `Cancelled`。

**并发策略**：活跃 Run 期间又来一批新照片 → **不猜，问一句**"并入当前，还是新开一单？"

## 七、架构落位与边界

**分层（依赖单向）**：
```
core/  (C++ 业务库，无终端/网络/编排概念)
  ▲   只调 core/api
cli/   (C++ 前端：交互 `pzt open` + 一组 headless 子命令)
  ▲   shell out 调子命令，JSON 进出
agent/ (Python 编排进程)
```
- 都在**同一个 PZT 仓库**：`core/`、`cli/`（含 headless 子命令，就是现有 `pzt` 子命令集的扩充，**不是新 lib**）、`agent/`（Python，独立 venv）。多语言构建正常。
- "平级兄弟"落地为：`agent/` 是 core 公开面的平级消费者，只是独立 Python 进程、经 headless 命令契约消费——依赖单向 `agent → headless cli → core`，core 永不反向、编排概念永不下渗。
- headless 子命令的形态举例：`pzt tag apply <image> <tag>`、`pzt eval <proj> --scope … --provider {gemini|claude|local} --auto-reject`、`pzt dedup <proj> …`、`pzt curate <proj> …`、`pzt export-images <proj> <images…> <folder>`。按 `image_path` 寻址（`find_image_by_path` 已有）；交互流里藏的"人的决定"（如超 cap 替换哪张）在 headless 侧变成显式策略参数。这些是给已有 `core/api` 套的薄壳，不是重新实现。

**agent/ 内部组件**：Transport 适配器（可插拔，统一收发接口；v1 = Telegram Bot API，另有 watch-folder 供开发/测试，WhatsApp 等其它 IM 属后续适配器）、会话路由、驱动器、Stage 实现、意图/调整 composer（LLM）、Run/Plan 状态存储（agent 自己的 SQLite/JSON，按 `run_id` 存、引 core 的 `project_id`，绝不进 core schema）。

**模型 provider 边界（划清 core 与 agent 各管哪种 AI）**：
- **照片分析 AI（打分/去重）= core 的事**：eval 是 core 能力，provider（云/本地）经 headless flag 选；加 `Provider::Local`（指 localhost Ollama）即让 `--provider local` 本地跑，agent 只管传 flag。
- **语言/创作 AI（写文案、解析意图/调整）= agent 的事**：core 没有"文案"概念，用 agent 自己的 Python LLM 客户端。
- **Apple Vision（确定性照片分析：水平角/显著性/embedding/分类/美学评分）**：概念归 core 侧，但 Vision 是 Swift/ObjC → 落地成一个 headless 命令（`pzt vision <op> --json`，Obj-C++ `.mm` 或独立小 Swift helper），agent 照样 shell out 调。
- 一句话切分：**core 管"看照片的 AI"（确定性分析、打分），agent 管"用语言的 AI"（创作、解析人话）**——正合 AGENTS.md 的 AI 边界。

## 八、错误处理与可恢复性

**失败分类 + 策略**：
- **瞬时/可重试**（云 LLM 抖动、Ollama 没就绪、临时 I/O）→ 有限次退避重试（按 `cost_class`，云端网络才需要），用尽再升级。
- **Stage 内逐项部分失败**（40 张评估，38 成 2 坏）→ **不算 Stage 失败**：best-effort、逐项报跳过（复用 M3 的 `ExportSkipped`/`last_failure`/skipped 计数），Run 照常往下，末端复核列出。
- **Stage 级硬失败** → 不杀整个 Run，按 `criticality`：关键 → `Failed`；非关键 → **降级**（跳过、带"能产出的东西 + 失败清单"走到 `AwaitingReview`）；拿不准 → 暂停成 gate 问用户。
- **崩溃/离线** → 重载非终态 Run、从下一个 pending Stage 续跑；`AwaitingGate/Review` 的等待是持久的，等几小时几天、重启不丢。
- **额度/预算**（云端配额用尽/花费上限）→ 当硬失败那步暂停并告知。

**三条恢复原则**：
1. **Run 是耐久单位**：每个 Stage 边界持久化 `Run.state`，崩溃最多损失"重跑当前这一个 Stage"。
2. **能降级不要死**：失败尽量降级成"少点打磨的成品"而非杀 Run，带残缺结果 + 清单走到复核，人来定夺（正合"人保留最终拍板权"）。
3. **暴露不吞**：每个跳过/失败都报给用户，复用 M3 结构化上报，绝不静默。

**错误处理住哪**：驱动器管编排层面的错误（重试计数、标 failed/skipped、按 criticality 决定降级/中止/暂停、持久化）；错误的内容由 headless 命令的 JSON 带回（复用 core 的 `Result<T,E>` 错误类型序列化）。

**唯一非幂等点**：`Deliver`（给用户发文件/消息）——崩在"已发送但没标 done"之间会重发。显式护栏：发前先持久化"本 Run 已交付"标记（或传输层去重），别把成片发两遍。

## 九、测试策略

**核心洞察**：本设计的确定性回报在这一节兑现——整套编排能在**没有 Mac Vision、没有 Ollama、没有真 IM（Telegram）、没有云 API、甚至没有真照片**的情况下单元测试。三个注入缝把所有"贵/外部"的挡在外面（延续 M3 到处 `HttpPostFn`/`EvaluationFn`/`DecodeFn` 注入的哲学）：

1. **驱动器（最大回报）—— 用假 Stage 测**：喂"假 Stage 计划"（返回预设的成功/失败/部分/跳过），断言：依赖顺序、闸门触发位置、timeout→proceed/hold、按 criticality 降级/中止/暂停、Stage 后持久化、**模拟崩溃重载续跑**、**调整→正确子图作废+重跑**。纯逻辑、无真 core/模型/网络。
2. **意图→Plan / 调整解析（LLM 步）—— 拆两半**：确定性校验护栏（纯逻辑，重点硬测）；LLM 解析本身用注入**假 LLM 响应**（复用 M3 的 `HttpPostFn` 缝）测映射逻辑 + 离线 **eval 集**（真实人话 → 期望 Plan 形状，偶尔跑真模型抓 prompt 回归、人工过、不进快测）。
3. **Stage 实现 & headless 命令**：headless 命令是 core 薄壳（core 已被 M3 测过）→ CLI 层 smoke 测；Stage 包装 → 注入**假子进程 runner**（返回预设 JSON），不需要真 `pzt`。
4. **整条对话流 —— 假传输 + 假 Stage 端到端**：喂**内存假传输**，脚本化整条"用户发图 + 意图 → plan → 闸门 → 回应 → 交付"，**整套骨架、零外部依赖**；含幂等/续跑测（各边界崩溃重载不双跑、**Deliver 不重发**）。
5. **v0 watch-folder 兼作真端到端**：本就是"监听文件夹 + 真 `pzt` 子命令 + 真评估"，天然一条真集成测。慢，跑得少。

**分层归属**：core 保持自己的 C++ 单测（不动）+ headless 子命令加薄 CLI smoke；agent（Python）一套 pytest（驱动器/校验/包装/对话流/续跑）；LLM prompt 质量单独离线 eval 集人工过；传输适配器（Telegram）本身按项目惯例走真机手动验证（像 cli 的 pty 验证），后面一切被注入缝挡住、可单测。

## 十、本 spec 之外的依赖与遗留（供后续 PRD / Eng Design 接手）

本工作流骨架依赖几处"别处的能力"，不在本 spec 内解决，需 PRD/Eng Design 处理：

- **`Curate` 的多样性策展算法**：全新能力（嵌入聚类/时间铺开/约束优化），设计见 `docs/M4_Brainstorm.md` 第六节。
- **`Style` 若含几何变换（裁切/水平矫正）**：会激活 M3 遗留的 recipe 几何变换缺口（`VersionParams` 目前只有色调/白平衡），是可能的前置依赖。
- **`Provider::Local`（core/ai）**：让 `pzt eval --provider local` 走本地 Ollama，本地模型分层设计见 `docs/M4_Brainstorm.md` 第五节。
- **headless 命令面**：给现有 `core/api` 补一批非交互 `pzt` 子命令（形态 B 的边界）。
- **agent 侧 Run/Plan 状态存储**：agent 自有持久化，独立于 core schema。
- **传输接入**：v1 已定 = Telegram 官方 Bot API（见 `docs/M4_PRD.md`）；WhatsApp 等其它 IM 作为后续可插拔适配器。控制/像素同通道、文件 vs 照片的思路沿用。
