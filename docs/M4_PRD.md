# PicZTream (PZT) Milestone 4 产品需求文档（Agent 半自动 · 增量一：粗筛交付最小闭环 + WhatsApp 接入）

## 背景与问题陈述

M0-M2（MVP / 风格化 / RAW）与 M3（选片辅助评分 + 近似重复检测）均已完成，`core/` 从一开始就要求"保持对未来非 cli 调用方（如 headless agent 层）的可复用性"——M4 正是那个被预留的调用方。

真实痛点：出去玩一天，手机拍了几十张，想留下能发的那几张，得自己一张张过、去重、剔废片、挑 keeper。即便有 PZT 的 TUI，也要打开、导入、逐张操作。用户想要的是：**把照片和一句话丢进一个聊天窗口，过一会儿就收到筛好的那几张**，全程不碰终端。

本增量把 M4 的 **agent 编排骨架**落地，并交付**第一个完整最小可用闭环**：WhatsApp 收图 → 云端评估 → 去重 → 策展挑 N 张 → 把 keeper 发回。它复用 M3 的评估/去重和 M0 的项目/标签/导出，新增的表面集中在编排、策展、headless 命令面和 WhatsApp 接入。

设计依据：`docs/M4_Agent_Workflow_Design.md`（工作流骨架 spec，权威）与 `docs/M4_Brainstorm.md`（广度参考：本地模型、用例、策展、产品岔路）。本 PRD 只写"本增量要交付的功能与可验收项"。

## M4 增量一目标（本增量范围）

1. **编排骨架**（照 spec 落地）：`Stage / Plan / Run` 三抽象、确定性驱动器、Run 生命周期状态机、per-Stage 检查点与断点续跑、可选闸门、意图触发、对话绑定与调整、降级式错误处理。
2. **v1 粗筛交付流水线**：`Ingest → Evaluate（云端）→ Dedup → Curate → Deliver`。不含风格/文案。
3. **`Curate`（新能力）**：从"通过评估 ∧ 非废片 ∧ 非重复"的候选里，多样性感知地挑出 N 张。
4. **headless 命令面（形态 B 边界）**：给 `core/api` 补一组非交互 `pzt` 子命令（JSON 进出、按路径寻址），供 agent 的 Stage shell out 调用。
5. **WhatsApp 接入**：按**文件**收原图（保 EXIF）+ 收意图消息；回进度、回交付的 keeper（文件）。控制与像素同一条通道。
6. **人在环**：末端统一复核 + 对话式微调（改张数、换掉某张）；agent 侧策略默认更激进（评估不达标直接打废片），仅作为 Stage 参数，物理隔离于人工路径的全局 Settings。

## 非目标（本增量不做）

- **完整朋友圈形态**：`Style`（统一风格）、`Caption`（文案生成）——见文末 TODO。
- **本地模型**（Tier 0 Apple Vision / Tier 1 Ollama）：评估先走云端——见文末 TODO。
- **多 Profile / 多目的地**（IG / 小红书差异化策展）。
- **brainstorm 里标 later 的用例**：检索式提问、增量续拍、批量整理、多目的地等。
- **替用户发布**：微信 Moments 无 API，agent 永远只交付素材、不代发。
- **watch-folder 之外的 IM**（Telegram 等）：本增量只做 WhatsApp（watch-folder 作为开发/测试期的等价传输保留）。

## 核心用户故事

出门玩了一天，手机拍了约 40 张。回到住处，把这些照片**按"文件"全发给** WhatsApp 上那个 PZT 账号，再发一句"帮我筛一下，留 9 张"。agent 回一句"好，40 张里帮你筛 9 张，开跑~"，随后回报进度（"评估 24/40…"）。跑完把 **9 张 keeper（原画质文件）+ 一句摘要**发回（"从 40 张里去掉 6 张重复、5 张不达标，给你挑了这 9 张"）。用户看了说"第 3 张换一张""其实留 12 张"，agent 只重挑、很快回新结果。满意就结束。

## 功能需求

### 触发与意图

- **意图消息触发**：照片经 WhatsApp 到达、无活跃 Run → 归到该用户一个 `Collecting` Run；意图消息（"筛一下留 9 张"）到达即解析、组装 Plan、开跑。照片与意图可任意顺序到。
- **静默问询 / 显式开始**：有图但久无意图 → agent 主动问"看到你发了 N 张，想怎么处理？"；有图且意图已知（先说要求再发图）→ 静默到点直接开跑；显式"开始/收工"随时兜底。
- **意图 → Plan 组装**：一次 LLM 调用（本增量走云端）把意图 + Profile 默认 + 上次配置组装成结构化 Plan（有序 Stage + 参数 + 闸门三态），随后一道**确定性校验**（Stage 存在、参数在范围、依赖可满足）挡住非法输出，不合法则修一次或退回默认并告知。
- 开跑回一句 plan 摘要（只知会、不阻塞）。

### 编排与状态机

- Run 生命周期：`Collecting → Planned → Running → AwaitingGate → AwaitingReview → Done / Failed / Cancelled`。
- 驱动器纯确定性：取 Stage → 查依赖 → 过闸门 → 调 headless 命令 → 写产物 → **每 Stage 后整份 `Run.state` 持久化**。
- 断点续跑：崩溃/离线后重载非终态 Run，从下一个 pending Stage 续，**评估等贵步骤跑过不再跑**。

### 闸门与人在环

- 本增量的默认闸门：**云端 `Evaluate` 前一个 courtesy 闸门**（几十张上云 = 花钱，抬头知会）；`Curate` 无闸门；末端 `AwaitingReview` 恒在。
- 每闸门三态可配：`off` / `courtesy`（问一声、超时默认继续）/ `required`（必须答）。全关 = 全自动，全 required = 步步盯。
- 末端复核发**内联照片预览**；approve 后才发**最终文件**。approve → `Done`；调整 → 重跑受影响子图 → 回复核；打回 → `Cancelled`。

### Stage 详述（本增量五个）

- **`Ingest`**：把 WhatsApp 收到的原图文件落到暂存目录、`create_project`（复用 core）。关键 Stage（失败则 Run `Failed`）。
- **`Evaluate`**：复用 M3 `EvaluationWorker` / `request_evaluation`（本增量 provider = 云端）。策略参数 `auto_reject`：agent 默认 **true**（不达标直接打废片），经 `pzt eval --auto-reject` 显式传入，**不改人工路径的 `Settings.auto_ai_reject`**。逐项部分失败（个别图解码坏）跳过并计数，不算 Stage 失败。
- **`Dedup`**：复用 M3 `find_and_tag_duplicates`。因按文件收原图、EXIF 保全，`captured_at` 在，时间聚类正常。
- **`Curate`（新）**：候选 = `passes_gate` ∧ 非废片 ∧ 非重复（复用 `passes_gate` / `images_with_tag`）。从候选挑 N 张，**多样性感知**：本增量用"去重分组 + `captured_at` 时间铺开 + `overall_score`"组合（不用语义 embedding——那是本地 Vision，属 TODO）。确定性可复现（同输入同输出）。落 core、经 `pzt curate` 暴露。给选中的打一个 `发布/精选` 标签（复用 `add_tag`）。
- **`Deliver`**：复用 `export_images` 导出成品；经 WhatsApp 回传。**唯一非幂等点**：发前先持久化"本 Run 已交付"标记（或传输层去重），不重发。

### 对话绑定与调整

- 消息归类**状态优先、内容其次**：Run 处于 `AwaitingReview` → approve / 调整 / 打回；`Running` → 插话捕获成待应用调整、下个 Stage 边界应用；无活跃 Run → 新 Run。
- 调整解析成**结构化配置增量 + 受影响 Stage 集合**："换掉第 3 张"→`Curate`（排除、补 1 张）；"留 12 张"→`Curate(count=12)`。驱动器**只作废受影响 Stage 及其下游、重跑子图**，不重跑上游的贵步骤（eval/dedup）。
- **老规矩（Q4）**：Run 完成时把最终 Plan+参数存成该用户"上次配置"，下次组装当默认。
- **并发**：活跃 Run 期间又来一批新照片 → 不猜，问"并入当前，还是新开一单？"。

### WhatsApp 接入

- 传输适配器在统一接口后（另有 watch-folder 实现供开发/测试）。收：意图文本、原图**文件**（保 EXIF）；发：文本、照片预览、文件。
- 控制与像素同一条 WhatsApp 通道：预览用内联照片（快、够瞄），原图输入与最终交付用文件（字节精确）。
- 进度回报复用 `ScanProgressFn` / `EvaluationWorker` 的进度模式。

### headless 命令面

- 给 `core/api` 补一组非交互 `pzt` 子命令，JSON 进出、按 `image_path` 寻址（`find_image_by_path` 已有）：`pzt eval <proj> --scope … --provider … --auto-reject`、`pzt dedup <proj> …`、`pzt curate <proj> …`、`pzt tag apply <image> <tag>`、`pzt export-images <proj> <images…> <folder>`（`pzt new` 已有）。
- 是给已有 `core/api` 套的薄壳；交互流里藏的"人的决定"（如超 cap 替换哪张）在 headless 侧变成显式策略参数。

## 非功能需求

- **确定性 & 可测**：驱动器零 LLM、纯确定性；LLM 只在意图/调整解析与 Plan 组装处；三个注入缝（假 Stage / 假子进程 runner / 假传输）让整套编排脱离真 core、真模型、真 WhatsApp 可单测。
- **分层**：`core` 保持无编排、无策略、无 LLM 感知；agent 是独立 Python 进程，经 headless 命令契约单向依赖 core；Run/Plan 状态存 agent 自有存储（不进 core schema）。
- **隐私**：本增量评估走云端 = 照片（降采样后，复用 M3 F-02 的上传前缩放）上云，需明确告知；"照片不出机"硬约束靠本地模型，属 TODO。
- **成本**：本增量逐张云端评估有成本，作为最小可用接受；成本优化（本地化）属 TODO。

## 技术方案概要

- 形态 B：`agent/`（Python，同仓、独立 venv）→ headless `pzt` 子命令（C++，`cli/` 内现有子命令集的扩充）→ `core`（C++）。
- 复用：`create_project` / `EvaluationWorker`（云）/ `find_and_tag_duplicates` / `passes_gate` / `overall_score` / `images_with_tag` / `add_tag` / `export_images`。
- 新增：`core` 侧 `Curate`（确定性策展，经 `pzt curate` 暴露）；一组 headless 子命令；`agent/` 全部（传输、路由、驱动器、Stage 实现、意图/调整 composer、状态存储）。
- provider 不变（本增量只云端）；`Provider::Local` 属 TODO。

## 验收标准

- **端到端**：WhatsApp 按文件发 N 张 + 一句意图 → 收到 keeper 文件 + 结果摘要；watch-folder 等价路径同样跑通。
- **Curate 正确**：候选严格是"通过评估 ∧ 非废片 ∧ 非重复"；输出恰为 N 张（候选不足时给出全部并说明）；多样性可见（不会把同一连拍的近重复塞满 N 张）；同输入可复现。
- **激进策略隔离**：agent 触发的评估默认给不达标打废片；**人工路径 `pzt open` 的行为与 `Settings.auto_ai_reject` 不受影响**（验证全局配置未被改动）。
- **断点续跑**：跑到中途杀进程再启，从下一个 pending Stage 续，已完成的评估**不重跑**。
- **调整只重跑子图**："换掉第 X 张 / 改张数"只重跑 `Curate→Deliver`，不重跑 `Evaluate`/`Dedup`。
- **幂等交付**：`Deliver` 在"已发送未标 done"崩溃后重启，不把 keeper 发第二遍。
- **EXIF 保全**：按文件收的原图 `captured_at` 存在，dedup 时间聚类正常（不出现整批 `skipped_no_capture_time`）。
- **headless 命令**：每个新 `pzt` 子命令能手敲运行、JSON 输出形状稳定。
- **降级不死**：非关键 Stage 失败（如个别图评估失败）Run 仍带残缺结果 + 失败清单走到复核，不静默、不崩。

## 风险与待确认问题

- **WhatsApp 官方 vs 非官方接入**：官方 Business Cloud API 合规但有审批/计费；非官方库（whatsapp-web.js / Baileys）好上手但违反 ToS、有封号风险。**倾向**：个人单账号自用场景先用非官方库快速跑通，明确记风险；若要长期/多用户再迁官方。**待用户拍板。**
- **文件发送的 UX 摩擦**：几十张按"文档"发比照片选择器多几步。缓解：预览环节用照片、只有原图输入与最终交付用文件；真机观察摩擦是否可接受。
- **Curate 多样性效果**：本增量不含语义 embedding（那是本地 Vision，TODO），多样性靠"去重分组 + 时间铺开 + 分数"，效果需真机观察，可能后续用 Tier 0 embedding 增强。
- **云端评估成本**：本增量接受；真实用几批后评估成本是否可忍，决定本地化 TODO 的优先级。

### TODO（规划中，本增量不做，后续增量接手）

- **本地模型分层**（Tier 0 Apple Vision：水平角/显著性/分类/embedding/美学；Tier 1 Ollama 小 VLM 作为 `Provider::Local`）→ 压掉逐张云端评估成本、支持"照片不出机"。详见 `docs/M4_Brainstorm.md` 第五节。
- **完整朋友圈形态**：`Style`（统一风格；若含裁切/水平矫正，需先给 recipe/渲染管线补几何变换能力，激活 M3 遗留缺口）+ `Caption`（多模态看图写中文社媒文案，云端或本地 Qwen）。
- **多 Profile / 多目的地**：IG / 小红书等差异化 Profile（张数、裁切比例、文案语气/话题标签）。
- **策展增强**：LLM 策展作为算法策展之上的可选增强。
