# 目标五 Eng Design：Agent 运行时 consumer/worker 双线程重构

## 一、背景与架构总览

`docs/W2026-07-15_PRD.md` 目标五：`run_telegram.py` 单线程同步主循环把整条 drive（Evaluate 数分钟）和同步 LLM 分类调用（本地 Ollama 首响 10s+）都嵌在 `handle_message` 里，期间入站消息全部堵死。重构为双线程 + 消息传递：

```
TelegramTransport(现有, 自带 asyncio 收发线程, 零改动)
        | InboundMessage 队列 (现有 transport._inbound)
        v
[consumer 线程 = 主线程]  0.5s 节奏循环, 每轮:
    drain transport.receive() -> 关键词快路径 / 存照片 / 文本串行入队 job
    drain event 队列 -> 更新 SessionView -> 渲染并发送对话文本
    timers: idle 提醒 / 收图播报 / RUNNING 期 Evaluate 进度轮询
        | Job 队列 (queue.Queue, FIFO, 带 generation)
        v                                   ^ Event 队列 (queue.Queue)
[worker 线程]  单队列串行执行:               |
    ClassifyJob / ComposeJob -> 纯 LLM 调用, 不碰 RunState
    DriveJob -> 循环 driver.advance(), 闸门尾部导出+发预览媒体, 事件上报
    cancel: job 上的 threading.Event, 子进程级终止 (见第六节)
```

职责边界：

- **consumer**：唯一渲染与发送**对话文本**的线程；持有 SessionView；非 DriveJob 活跃期间独占 RunState 的变更与落盘（collecting 存图、planned 改参数、timers 字段）。
- **worker**：串行执行 job；DriveJob 活跃期间独占对应 run 的变更与落盘（Driver 原样复用，其内部 `store.save` 就是断点续跑检查点）；在 job 内直接发送 **stage 产物媒体**（预览图；DeliverStage 自己的文件发送与"选好了 N 张"文本维持原样，见第九节）；不做任何会话决策。
- **SessionView**：consumer 私有内存视图，事件驱动更新，重启时从 RunStore 重建。它是"当前到哪一步/进度多少"的快速应答来源，**不是**持久真相。

线程实现用 `threading.Thread(daemon=True)` + `threading.Event` 停止信号（Python 侧对 `std::jthread` 精神的对应：生命周期被 owner 持有、协作式停止；C++ 契约本身不约束 agent 层）。

**考虑过并否决的替代**：全 asyncio 单线程（取消天然、无锁）。否决原因：Driver/stages/store 全是同步风格且被 run_intent/run_watchfolder 共用，async 化改造面大；双线程方案对现有代码形状是加法不是改写。

## 二、所有权协议：RunState 唯一真相与"落盘 + 事件"交接

`RunState` + `RunStore` 仍是唯一持久真相。`RunStore.save` 是整文件覆盖写、无合并语义，因此**同一时刻只允许一个线程持有某个 run 的可变引用**：

- **交接方式**：不共享内存对象。consumer 把 run 落盘后，DriveJob 只携带 `run_id`；worker 从 store `load` 出自己的实例开始推进；drive 停下（闸门/终态/异常）时 worker 确保已落盘、发事件；consumer 收到事件后需要读 run 时重新 `load`。交接点永远是"落盘 + 事件"，天然崩溃一致（崩在交接前后都能从盘上恢复）。
- **consumer 独占期**（status ∈ COLLECTING/PLANNED/AWAITING_GATE/AWAITING_REVIEW 且无活跃 DriveJob）：现有 router 的全部写路径（last_activity_at、reminder_sent、plan 参数、状态迁移）原样归 consumer。
- **worker 独占期**（DriveJob 活跃）：consumer 不 load、不 save 该 run，一切应答只依赖 SessionView。现有代码天然契合——旧 router 在 RUNNING 态本来就不写 run（`_dispatch` 的活跃时间刷新只覆盖 COLLECTING/PLANNED/AWAITING_GATE 三态）。
- **跨期共享的只有两样**：job 上的 `cancel_event`（threading.Event，天然线程安全）和 event 队列（queue.Queue）。

## 三、`session/protocol.py`：Job/Event schema 与 generation

```python
@dataclass
class Job:
    generation: int
    cancel_event: threading.Event  # 每个 job 自带; 取消 = set()

@dataclass
class ClassifyJob(Job):
    kind: Literal["collecting", "gate_reply", "refine_plan"]
    text: str
    context: dict          # collecting: {photo_count}; gate_reply: {run_id};
                           # refine_plan: {intent_raw, current_params}

@dataclass
class ComposeJob(Job):
    intent_text: str       # 结果是已 validate_plan 的 Plan

@dataclass
class DriveJob(Job):
    action: Literal["start", "resume", "resolve_gate", "adjustment", "rerun_style"]
    run_id: str
    args: dict             # adjustment: {delta}; rerun_style: {style_description}
```

事件（worker -> consumer，全部带 `generation` 原样回传）：

```python
ClassifyDone(generation, kind, result)      # result = 对应 parser 的返回对象
ClassifyFailed(generation, kind, retryable) # AdjustmentError -> retryable=False(没听懂);
                                            # LlmRequestError -> retryable=True(基础设施故障)
ComposeDone(generation, plan)
ComposeFailed(generation, message)          # ValidationError/LlmRequestError 的 message
StageStarted(generation, run_id, stage)
GateReached(generation, run_id, stage, payload)         # payload 见第五节
RunFinished(generation, run_id, status, detail)         # DONE/FAILED/CANCELLED; detail=失败明细
JobCrashed(generation, error)               # worker 内未预期异常的兜底
```

**generation 语义**：consumer 维护会话代数，每次"取消"生效或 run 终结时 +1。consumer 收到事件先比对 generation，过期事件直接丢弃——解决"classify 还在跑、用户已取消，结果回来把新会话状态搞脏"这类陈旧回调问题。同一代内 job 串行（单 worker 队列），不需要更细的排序号。

## 四、`session/view.py`：SessionView

```python
@dataclass
class SessionView:
    run_id: Optional[str] = None
    project_id: Optional[str] = None
    status: Optional[RunStatus] = None
    current_stage: Optional[str] = None          # StageStarted 更新
    stage_progress: Optional[tuple[int, int]] = None  # (done, total)
    gate_stage: Optional[str] = None             # GateReached 更新
    plan_summary: Optional[dict] = None          # PLANNED 时复制 provider/count/apply_tag/auto_reject
    selected_count: Optional[int] = None         # GateReached payload 复制
    drive_active: bool = False                   # DriveJob 入队 True, 终态/闸门事件 False
```

- `photo_count` 不是字段：照片数 = `incoming_dir_for()` 目录计数，惰性现算（旧 router 同款），避免与目录真相漂移。
- `describe()`：`_status_snapshot_text` 的迁移与扩展——COLLECTING/PLANNED 沿用旧文案；新增 RUNNING 分支（"正在{stage}…已完成 N/M"），全部从视图字段渲染，**不触碰 worker 独占期的 RunState**。
- 用户提出的 `ai_eval_in_progress` 这类步骤枚举**不单独建**：`(status, current_stage, stage_progress)` 组合已完整表达，避免平行枚举漂移。
- 重启重建：`store.list_active()` 取唯一活跃 run，从 RunState 抄 status/plan 参数；current_stage/进度留空，由续跑事件重新填充。

## 五、`session/worker.py`：执行器语义

`SessionWorker` 持有：job 队列、event 队列、自己的 `PztClient` 实例、Driver、store、transport（只用于发媒体）、compose/classify 函数注入（与旧 router 相同的注入缝）。核心是 `step(timeout)`（取一个 job 执行完），线程 `run()` 只是 `while not stopped: step()` 的薄壳——测试单步驱动不起线程。

**ClassifyJob/ComposeJob**：纯函数调用（`classify_collecting_message`/`classify_gate_reply`/`refine_plan_confirmation`/`compose_plan`+`validate_plan`），异常按第三节映射成事件。不碰 RunState、不发任何消息。

**DriveJob**：

1. `store.load(run_id)` 取得独占实例；按 action 先行操作：`start`→置 RUNNING（consumer 已置好、这里兜底）；`resolve_gate`→`driver.resolve_gate(run, "proceed")`；`adjustment`→`driver.apply_adjustment(run, delta)`；`rerun_style`→`driver.rerun_stage(run, "Style", {...})`；`resume`→直接进推进循环。
2. 推进循环（旧 `_drive_to_stop` 的迁移）：`while run.status == RUNNING`：先查 `cancel_event`（置位→`driver.cancel(run)`、落盘、发 `RunFinished(CANCELLED)`、返回）；`peek_next_stage()` 发 `StageStarted`（consumer 负责渲染"正在评估…"等旧文案，Style/StyleApplyAll 不发进度文案的旧规则由 consumer 判断）；对可杀 stage 布防 `cancel_event`（见第六节）；`driver.advance(run)`。
3. 停在闸门：按闸门 stage 做**媒体准备与发送**（旧 `_send_style_preview`/`_send_preview` 的导出+发图部分迁到这里——`export-images` 子进程 + `send_photo`/`send_file` 逐张降级重试），然后发 `GateReached`，payload 携带 consumer 渲染文案所需的全部字段：
   - Style 闸门：`{}`（无媒体，consumer 直接问"想要什么风格"）
   - StyleApplyAll 闸门：`{chosen_recipe, preview_sent: bool, export_error: Optional[str]}`
   - Deliver 闸门：`{selected_count, applied_recipe: Optional[str], preview_failed_count, export_error}`
4. 停在 AWAITING_REVIEW：`driver.approve(run)` 后发 `RunFinished(DONE)`（旧 router 各路径的自动 approve 对齐）。FAILED：发 `RunFinished(FAILED, detail=_first_failure_detail 迁移)`。
5. 未预期异常：兜底 `JobCrashed`；run 停在最后一次落盘的检查点（多为 RUNNING），依赖"下一条用户消息触发 resume"恢复，对齐旧 router 崩溃续跑语义，不自动重试（防崩溃循环）。

**文本/媒体分工的准确表述**：对话文本一律 consumer 渲染发送；stage 产物媒体（预览图字节）由 worker 在 job 内顺序发送，发完才发事件，consumer 的接话文本因果地排在媒体之后，消息顺序不会交错。唯一例外是 DeliverStage 内部既有的 `send_file` 循环与"选好了 N 张"文本——它被三个入口共用、带逐张 marker 断点，本次零改动（见第九节）。

## 六、取消协议与 `pzt_client.py` 可取消调用

**consumer 侧**：任何状态收到 `_REJECT_KEYWORDS` 关键词，秒回回执：

- 无活跃 DriveJob（COLLECTING/PLANNED/AWAITING_GATE）：consumer 直接 `driver.cancel(run)` + "已取消"（COLLECTING 态是新增路径：旧代码会把"取消"当意图丢给 compose_plan；取消时已收照片随 run 废弃，不并入 `_pending`——用户明确说不要了）。
- DriveJob 活跃：回"正在停下来…"，set `cancel_event`，generation +1；等 worker 的 `RunFinished(CANCELLED)`（过期 generation 也接受这一种事件用于确认停止，或 consumer 在发起取消时就地更新视图为 CANCELLED、后续事件全丢弃——实现取后者，更简单且 UI 即时）。

**worker 侧**：两个检查点。stage 边界（推进循环每轮开头）必查；子进程内部只对可杀 stage 生效：

- **可杀白名单 = {Evaluate, Dedup}**：仅有的分钟级 stage。`pzt eval` 逐张落库、重跑自动跳过已评估；dedup 可重算——SIGTERM 安全（SQLite busy_timeout=5000 + 事务，最坏回滚最后一笔）。
- **其余 stage 跑完为止**：Ingest（`pzt new` 杀了会留半个项目、重跑撞已存在）、Curate/recipe apply/export（秒级，DB 写或本地导出）、StyleApplyAll（逐张 `recipe apply` 纯 DB 指针写、整段秒级——PRD 里"循环边界生效"一句按此修正，见文末）、Deliver（必选闸门确认后才开始，且有逐张 marker，中断语义交给既有断点续传）。

**`PztClient` 改动**：实例新增 `cancel_event: Optional[threading.Event]` 布防点（挂在实例上而不是 `call()` 参数——stages 内部的 `client.call(...)` 才能零改动吃到取消能力）。未布防时 `call()` 行为与现状完全一致（`subprocess.run` 路径不动，兄弟入口零影响）。布防时走 `Popen`：`communicate(timeout=0.1)` 轮询（超时重试不丢输出），`cancel_event` 置位→`terminate()`，2s 宽限后 `kill()`，抛 `PztCancelledError`（新异常，**不是** `PztCommandError` 子类——stage 的 `except PztCommandError` 不会吞它，它穿透 stage.run 和 driver.advance 直达 worker 的推进循环，worker 捕获后走 cancel 收尾）。测试注入：现有 `runner` 缝保留给非取消路径，新增 `popen_factory` 缝给可取消路径。

worker 布防方式：推进循环里 `peek_next_stage()` ∈ 白名单时把 job 的 `cancel_event` 挂到自己 client 实例上（worker 专属 client，consumer 用另一个实例做只读查询，互不影响），advance 返回后摘除。stages 代码零改动。

## 七、`session/consumer.py`：消息处理规则

`SessionConsumer` 持有：transport、store、driver（仅用于非 drive 期的 cancel/approve 等即时小操作）、view、job/event 队列、只读 `PztClient`、timers 配置。主循环 `step()`：drain inbound → drain events → timers，由 `run_telegram2.py` 以 0.5s 节奏调用。

**入站分派（按优先级）**：

1. **chat_id 过滤**（旧规则）。
2. **照片/文件**：永不排队等 LLM。COLLECTING/PLANNED→`stage_incoming_photo`（旧行为，逐张不回复）；AWAITING_GATE→`queue_incoming_photo` + "先帮你收着…"（旧行为）；**RUNNING（新可达）**→同 AWAITING_GATE 排入 `_pending`；无活跃 run→mint collecting run（含 `drain_queue_into` + "之前排队的 N 张已经并进这一批了"，旧行为）。
3. **取消关键词**：见第六节，跳过文本串行队列。**只有取消跳队**——"好的"这类同意词不跳：若 refine 结果未回就抢跑"好的"，会拿旧参数开跑，语义错。
4. **其余文本**：进 per-session 文本 FIFO。队头出队规则：当前无 in-flight 的 Classify/Compose job 才处理下一条；处理动作按 view.status 分派，与旧 router 各 `_handle_*` 一致，只是"LLM 调用点"全部换成投递 job + 等事件：
   - COLLECTING：先关键词（同意词在此无意义、不特判），投 `ClassifyJob(collecting)`；`ClassifyDone(query)`→`describe()` 应答；否则投 `ComposeJob`；`ComposeDone`→写 plan/参数注入（Ingest folder、Deliver out_folder、Deliver gate=required，旧 `_propose_plan` 原样）→PLANNED+确认文案；`ComposeFailed`→"没看懂这句意图…"留在 COLLECTING；`ClassifyFailed`→按旧行为直接当意图走 Compose（分类只是锦上添花）。
   - PLANNED：同意词→置 RUNNING、落盘、"开始处理了，共 N 张"、投 `DriveJob(start)`；其余投 `ClassifyJob(refine_plan)`，结果按旧 `_handle_planned` 的 clarify/query/approve/reject/confirmed 分支处理（confirmed→改参数重发确认，不自动开跑，旧拍板保持）。
   - AWAITING_GATE：Style/StyleApplyAll 闸门走旧 `_handle_style_gate` 逻辑（自由文本即描述，无 LLM 分类），动作映射为 `DriveJob(rerun_style)` / `DriveJob(resolve_gate)`；普通闸门先关键词再投 `ClassifyJob(gate_reply)`，approve→`DriveJob(resolve_gate)`，adjustment→`DriveJob(adjustment)`，query→`describe()`。
   - **RUNNING（新可达）**：不做 LLM 分类（单 worker 正被 drive 占用，排队等于饿死）。模板应答：任何非取消文本→`describe()`（"正在评估…已完成 N/M，说'取消'可以停"）。
   - AWAITING_REVIEW：旧 `_dispatch` 行为保持——approve 掉旧 run、mint 新 collecting run、当前消息按新 run 重新分派。
5. **事件应用**：`StageStarted`→更新视图 + 旧 `_STAGE_PROGRESS_MESSAGES` 文案（Style/StyleApplyAll 不发，旧注释里的自相矛盾规避保持）；`GateReached`→视图 + 按 payload 渲染旧闸门文案（"想要什么风格？"/"这是用「X」套用的效果…"/"选好了 N 张…"含预览失败计数与风格总结，文案逐字沿用）；`RunFinished(FAILED)`→"处理失败：{detail}"；`RunFinished(DONE)`→不发额外消息（Deliver 自己已说"选好了 N 张"，旧行为）；`RunFinished(CANCELLED)`→已在取消回执时应答过，仅更新视图；过期 generation 一律丢弃。
6. **timers**（旧两个 + 新一个）：
   - idle 提醒：旧 `check_idle_timers` 原样（300s、活动重置、一次性），仅作用于 COLLECTING/PLANNED/AWAITING_GATE。
   - 收图播报：旧 `check_progress_updates` 原样（60s 节奏、COLLECTING 限定）。
   - **Evaluate 进度轮询（新）**：view 为 RUNNING 且 current_stage==Evaluate 时，每 60s 用只读 client 跑 `pzt images <project> --json`，统计 `evaluated==true` / 总数，播报"评估进行中，已完成 N/M 张"。失败静默跳过（SQLite 并发读被 busy_timeout 保护，偶发失败无害）。这是 consumer 直接调 pzt 的唯一例外，限定为**只读、亚秒级**查询。
7. **启动恢复（新）**：构造时扫描 `list_active()`：RUNNING→重建视图、发"上次处理被中断，正在接着跑…"、投 `DriveJob(resume)`；COLLECTING/PLANNED/AWAITING_GATE→仅重建视图（闸门提示不重发，靠 idle 提醒兜底，避免重启刷屏）。

**单活跃 run 语义**：旧 `assert len(active) <= 1` 保持。RUNNING 期间新批次照片进 `_pending`，run 终结后下一条消息 mint 新 run 时并入——与旧 AWAITING_GATE 行为对齐，多 run 并发明确不做。

## 八、与旧 router 的行为对齐清单

并行验证期的对照基准，2.0 必须逐条等价（文案逐字一致）：

| # | 行为 | 旧实现位置 |
|---|---|---|
| 1 | chat_id 白名单过滤 | `handle_message` |
| 2 | 单活跃 run 断言 | `handle_message` |
| 3 | AWAITING_REVIEW 来消息→approve+mint 新 run | `_dispatch` |
| 4 | 活动时间刷新+提醒标记复位（三态限定） | `_dispatch` |
| 5 | mint 时 drain `_pending` + 并批提示 | `_mint_collecting_run` |
| 6 | collecting 收图逐张不回复 | `_handle_collecting` |
| 7 | collecting 问询→快照文案 | `_handle_collecting` |
| 8 | 分类失败降级为直接当意图 | `_handle_collecting` |
| 9 | 意图→Plan 参数注入+Deliver 必选闸门+确认文案 | `_propose_plan` |
| 10 | PLANNED 确认/拒绝/调参回显不自动跑/clarify/query | `_handle_planned` |
| 11 | 闸门收图排队+"先帮你收着" | `_handle_gate` |
| 12 | 普通闸门 同意/取消/调整/query 四分支 | `_handle_gate` |
| 13 | Style 闸门自由文本即描述、空回车引导 | `_handle_style_gate` |
| 14 | StyleApplyAll 闸门 同意/新描述重跑 | `_handle_style_gate` |
| 15 | stage 进度文案（Style/StyleApplyAll 豁免） | `_drive_to_stop` |
| 16 | 闸门预览：导出→逐张发图→降级发文件→失败计数 | `_send_preview`/`_send_style_preview` |
| 17 | 预览总结含风格名+失败数+引导语 | `_send_preview` |
| 18 | 失败文案"处理失败：{stage}：{error}" | `_drive_to_stop_and_notify`/`_first_failure_detail` |
| 19 | idle 提醒三态文案、一次性、活动重置 | `check_idle_timers` |
| 20 | 收图 60s 播报、首播满间隔 | `check_progress_updates` |
| 21 | RUNNING 崩溃后可续跑 | `_dispatch` RUNNING 分支（2.0 增强为启动自动续跑） |

新增行为（旧实现不存在，PRD 验收标准来源）：collecting 态取消、RUNNING 态照片排队/模板应答/秒级取消、Evaluate 量化进度播报、启动自动续跑。

**真机反馈的刻意偏离（2026-07-17 首轮真机后，切换时勿"还原"成旧文案）**：
- **可观测性**：2.0 重写掉了旧主循环的 `[收到消息]` 等 print，导致真机跑起来终端是黑盒。consumer/worker 全程 print 关键事件（收消息/投 job/收事件/回复、worker 执行 job/运行 stage/崩溃全栈），对齐旧入口的调试体感。
- **JobCrashed 不再静默**：worker 未预期异常除打全栈外，consumer 收到 `JobCrashed` 会回一句话（drive 中"这批先停在这儿了，回句话我接着试"／分类中"刚才那条没能处理"），不再只 print。
- **确认/快照文案去掉 provider**：评估 provider 由 Settings 决定，对用户是无用信息，`_send_plan_confirmation` 与 `describe()` 都不再显示（provider 仍保留在 plan 参数里，refine 想改仍可改）。对齐清单第 9、10 条的文案据此更新。
- **compose/分类失败区分基础设施错误**：命中 `network_error`/`http_error`/`missing_api_key` 等痕迹时回"AI 服务好像连不上"，而非误导性的"没看懂这句意图"。
- **apply_tag 依目的地/受众取名**（`compose/plan_composer.py` prompt）：给出"发朋友圈→朋友圈、发ins→ins"等具体例子，只有完全没提目的地/受众/标签时才回落默认"精选"，纠正真机上"标签永远叫精选"的问题。
- **所有 yes/no 确认点改 inline 按钮**（真机反馈第二轮）：起因是风格闸门用精确关键词匹配、非关键词一律当"新风格描述"，用户回"不错"/"ok好的"被当描述丢给 style_matcher → `hallucinated`。改法是给四个确认点挂 Telegram inline 按钮：方案确认 `[好的][取消]`、Style 问描述 `[取消]`（开放式，无批准）、StyleApplyAll 预览 `[满意][重选][取消]`、Deliver 选图 `[满意][取消]`。实现分层：
  - **transport**：`InboundMessage` 加 `kind="callback"` + `data` 字段；`TelegramBotClient` 加 `send_text_with_buttons`（`InlineKeyboardMarkup`）与 `answer_callback_query`；`TelegramTransport._handle_update` 优先处理 `callback_query`（应答消 loading，转成 `kind="callback"` 入站），并加同步桥 `send_buttons`。`send_buttons` 是**可选能力**，不进 `Transport` Protocol 必需集，`WatchFolderTransport` 等不实现。
  - **consumer**：`callback_data` 拼成 `"{action}:{run_id}"`，点击回来校验 `run_id`（换 run 或已进 drive 的旧按钮一律"过期"，防误触）；approve/reject/restyle 映射到与打字关键词完全一致的处理路径。打字仍照旧（关键词批准/自由文本调整/重描述），按钮只是额外的无歧义快路径。`_send_buttons` 在 transport 无按钮能力时降级为纯文本，非 Telegram 入口不受影响。
  - **未做**：不改已发出消息的按钮态（点完不回收/置灰旧按钮），靠 `run_id` 过期判定兜底；够用，真机若嫌旧按钮可点再议。

## 九、复用与不动清单

**零改动**：`orchestrator/driver.py`（含 `peek_next_stage`）、`orchestrator/types.py`、全部 `stages/*`（DeliverStage 的 transport 发送与逐张 marker 原样）、`store/run_store.py`、`router/collecting.py`（纯函数，2.0 直接 import）、`compose/*`、`transport/*`、`run_intent.py`、`run_watchfolder.py`、core/cli 全部。

**修改**：`pzt_client.py`（加可取消路径，默认路径不动）。

**新增**：`session/{protocol,view,worker,consumer}.py`、`run_telegram2.py`、`tests/session/`。

**并行期约束**：`router/session_router.py` 及其测试原样保留；两个入口不能同时连同一个 bot（getUpdates 单消费者，双开会 409）。真机验证通过后 run_telegram2.py 转正、删旧 router 与旧入口测试中不再适用的部分（可迁移断言先移植）。

## 十、测试策略

- **不起真线程**：consumer/worker 都实现 `step()`，单测直接单步驱动 + 手工搬运两个队列，时间用注入 `now_fn`（旧 router 测试同款）。真线程只在 `run_telegram2.py` wiring 里，配一个短冒烟测试。
- **fakes 复用**：`tests/router/router_fakes.py` 的 FakeTransport/fake 分类函数/`PztClient` fake runner 模式平移到 `tests/session/`；可取消路径新增 fake `popen_factory`（可控 poll/terminate 时序）。
- **重点用例**：job 串行与文本 FIFO（refine 未回时"好的"不抢跑）；取消跳队 + generation 过期事件丢弃；可杀 stage 布防/摘除与 `PztCancelledError` 穿透；闸门事件 payload → 文案渲染逐字对齐第八节；RUNNING 态照片进 `_pending`、模板应答；启动恢复三分支；Evaluate 轮询播报（fake client 返回递增 evaluated 计数）与失败静默；崩溃兜底 `JobCrashed` 后下一条消息 resume。
- **端到端**：`PZT_FAKE_EVAL=1` 无网跑完整 collecting→确认→drive→双风格闸门→deliver 流程（对齐现有真机验证脚本的用法）。

## 十一、已知取舍与后续开放项

- **消息顺序的弱保证**：worker 发媒体与 consumer 发文本分属两线程，仅靠事件因果排序。timers 触发的播报可能插在媒体序列中间——接受为聊天流的正常形态，不做全局出站队列；若真机观感差再立项。
- **取消的最坏时延**：不可杀 stage（Ingest/Curate/export/Deliver）期间取消要等该 stage 完成，均为秒级~网络上传级；分钟级的 Evaluate/Dedup 已可杀。PRD 中"StyleApplyAll 循环边界生效"修正为"跑完为止"——它是逐张 DB 指针写、整段秒级，为它改 Stage 契约（注入取消回调）不值得。
- **Dedup 量化进度**：无逐张落库可查，且通常秒级，只给 stage 级文案。`pzt eval` 结构化进度协议这一 M4 开放问题继续开放（被轮询绕开，不再紧迫）。
- **多会话/多 chat_id**：SessionView/consumer 按单会话写死（对齐单 run 断言），多会话留给部署周立项时再泛化，不预留半成品抽象。
