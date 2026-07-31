# PZT 产品需求文档：headless 长运行的进度、降级信号与取消

- **来源**：`docs/proposal-2026-07-25.md` 提案 T-8（User 视角 U-10 + Architect 视角 A-7 收敛条目，P1 / size M / 无依赖）
- **日期**：2026-07-31
- **状态**：**已收口归档（2026-07-31，真机验收通过）**

---

## 归档说明

全部 7 项目标落地，验收标准 20 条全过。核心逻辑 core 325 / cli 69 doctest + agent 442 pytest 全绿（起点 393，本条 +49）。

**真机验收推翻了决策三**，返工两轮，详见正文里划掉的那一节。两轮的共同教训是同一条：**把维度压掉换来的"简洁"，代价会在展示层以说错话的形式付出。**

- 第一轮：压掉 `phase` 之后展示层只剩写死的单位"张"，而三个来源数的分别是候选簇、比较次数、照片。真机原话"正在筛选，已完成 1/1 张"——用户要选 3 张，那个 1/1 是 1 个候选簇，单位和主语双错。修法是让单位跟着数字一起传（`(done, total, kind)`），prose 仍全部留在 view 层。
- 第二轮：节流会把最后一跳吃掉（`2/3 → 3/3` 中间往往只隔几十毫秒，必然落在窗口内），且报到 `3/3` 时那句话仍是"正在…"。修法是终态跳不节流，外加在"上一个 stage 确实跑完了"的三个时刻把同一条进度消息改写成终态；取消与失败刻意不改写，那是撒谎。

**决策七（curate 不追求与 dedup 对称）经住了实现**。只补两个有消费者的进度回调，`on_ai_gate`/`on_cancel`/`ai_declined`/`cancelled` 仍不补；那条"将来谁加 gate 或 cancel 必须同时补上两个返回字段"的触发条件已写进 `core/curate/curate.h`，不只留在文档里。

**实现期发现的两件事**（都记进了 Eng Design）：

1. **管道死锁风险不是假设的**。`core/dedup/dedup.cpp:272` 对候选簇内每一对比较都无条件往 stderr 打一行明细，量级 Σ C(簇大小,2)，几十个中等簇就能越过 64KB 管道缓冲区。dedup 今天就在大量写 stderr，只是 `communicate` 一直在替我们排水。A.4 的两条回归测试因此用真子进程灌 5000 行。
2. **`dedup.cpp:266-271` 一处注释失准**：称"默认路径下 stderr 整个被重定向到 /dev/null"，那只对 `pzt open` 的 TUI 路径成立（`DebugLogRedirect`），headless 没有这层重定向。未改代码（那些明细行确实有用），读取方按"非 JSON 行是正常情况"处理。

**顺便攒下的数据点**：本地 Ollama 上单次 pairwise 视觉推理实测约 40s（8 张 4 簇的项目跑 2 分钟才到 3/4）。这是 T-11（锦标赛质量与成本基线）需要的三个数字之一。

---

## 背景与问题陈述

`W2026-07-21` 把 AI 锦标赛收进 core 之后，一次 `pzt dedup --ai` 内部要跑 Σ(簇大小 - 1) 次视觉推理，每次受 `core/ai/ai.cpp` 的 `CURLOPT_TIMEOUT 60L` 约束，整条命令的量级从秒级跳到分钟级。

这个变化在**坐在终端前的人**那一侧已经被连续收口过四次（T-9a 信号还原、T-9b 中途取消、T-14 的 `/dedup --ai` 两段进度、T-10 环境预检）。**Telegram 那一侧一次都没有收过**：用户确认方案后收到一句"正在筛选..."，然后是分钟级的彻底沉默，期间既不知道跑到哪了、也不知道能不能停、结束后还分不出结果是 AI 真跑通的还是超时退化的。

三条子缺陷，逐条对着 2026-07-31 的代码复核过，全部仍然成立：

### (a) 进度完全丢失

`core` 侧的进度钩子是齐的：`dedup::DedupProgressFn`（本地分簇，`done/total`）与 `dedup::AiProgress`（AI 阶段，组号 + 比较次数两级）都已铺到 `tournament::cluster_and_choose`，`tournament.h:60-63` 还特意注明"不能悄悄把它吞掉"。断点在 cli：

- `cli/commands/commands.cpp:202`，`cmd_dedup` 传 `/*on_progress=*/nullptr`，且 `on_ai_gate`/`on_ai_progress`/`on_cancel` 三个参数根本没传（吃默认 `nullptr`）。
- `cmd_curate`（`commands.cpp:534-537`）更靠前一步就断了：**`core::curate::curate` 的签名里就没有这几个回调**（`core/curate/curate.h:44-48`），`curate.cpp:104-107` 调 `cluster_and_choose` 时也不传。dedup 是"有钩子不传"，curate 是"连钩子都没有"。
- 于是 agent 侧对应地空着：`agent/session/protocol.py:84-86` 明写"注意没有 StageProgress 事件"；`consumer.py:541,867` 只把 `view.stage_progress` 赋 `None`；`view.py:74-75` 依赖它的分支是死代码；`agent/tests/session/test_view.py:137` 是唯一一处给它赋过非 None 的地方。
- 连带一处文案不实：`agent/README.md` 把 `--progress-interval-seconds` 说成"进度播报节奏"，但 `_check_collecting_progress`（`consumer.py:1018-1021`）`if run.status != RunStatus.COLLECTING: return`，它只管收图阶段。`_check_idle_reminder` 同样跳过 `RUNNING`（`consumer.py:994-1014`）。**运行期一条周期性消息都没有。**

### (b) 降级信号在 Curate 路径上丢失

`ai_fallback_count`（整簇 AI 比较失败、退化成"按拍摄时间选最新"的簇数）由 `commands.cpp:211`（dedup）与 `:558`（curate）都发出。

- `agent/stages/dedup.py:27` 是 `return StageOutput(ok=True, data=result)`，整个 dict 原样带过，字段活着。
- `agent/stages/curate.py:59-63` 重建了一个只含 `requested`/`returned`/`selected` 的新 dict，**字段就此蒸发**。全仓 grep 确认 `ai_fallback_count` 在 agent 生产代码里零命中。

后果：整簇因超时退化时，用户拿到的话术和 AI 真跑通时完全一样。这是"看起来 AI 帮我选了"和"其实是按时间选的"之间的静默替换。

### (c) 取消只覆盖一半

`agent/session/worker.py:47` `KILLABLE_STAGES = ("Dedup", "Curate")`。布防机制本身是 per-stage 的（`worker.py:242-244`，`armed = next_spec.name in self.killable_stages` 时把 `cancel_event` 挂到 client 实例上），加 stage 只是改这个元组。没覆盖的两个：

- **`Style`** 内部走 `pzt recipe suggest`，一次视觉推理，同样受 60s 超时约束。
- **`StyleApplyAll`**（`agent/stages/style_apply_all.py:33-38`）是一个 N 次 `pzt recipe apply` 子进程的 Python for 循环。30 张精选就是 30 次进程启动，实测本机纯进程启动 4.4ms/次，加上每次内部开 4 次库。

这条不对称至今成立：机器人能掐掉正在跑的 Dedup，套风格套到一半的用户不能。

---

## 与既有文档的冲突

`docs/SPEC.md` §3.2 是硬契约：

> 每个 headless 命令是一次提交、一次收尾的原子调用，**不做流式输出**；agent 侧把它当作确定性的子进程边界来编排。

按 §4.6"任何代码实现与权威文档冲突时以文档为准，如需偏离必须先提出并等待确认"，(a) 这一半必须先解决契约问题再动手。**已提出并拍板，见下面的决策一。**

(b) 与 (c) 不碰任何契约。

---

## 目标（本增量范围）

1. **G1 跨进程进度**：`pzt dedup --ai` 与 `pzt curate --ai` 在运行期把两级进度（本地分簇的 `done/total`、AI 阶段的组号与比较次数）送达 agent。
2. **G2 进程内进度**：`StyleApplyAll` 的 i/N 送达 agent（这一条不经跨进程通道，见决策四）。
3. **G3 播报**：agent 在 `RUNNING` 期间按节流周期把进度播给 Telegram 用户，`SessionView.stage_progress` 从死字段变成活字段，`view.py:74-75` 的分支变成活代码。
4. **G4 降级信号**：`ai_fallback_count` 在 Curate 路径上不再蒸发，且在用户可见话术里体现出来。
5. **G5 取消补全**：`Style` 与 `StyleApplyAll` 进入 `KILLABLE_STAGES`，用户在这两个阶段说"停"能真的停下。
6. **G6 curate 拿到进度钩子**：`curate::curate` / `api::curate_images` 补上 `on_progress` 与 `on_ai_progress` 两个参数并转发给 `cluster_and_choose`（决策七；其余四项不补）。
7. **G7 文档收口**：`agent/README.md` 关于 `--progress-interval-seconds` 的说法与实际行为一致；`docs/SPEC.md` §3.2 的契约表述更新。

---

## 非目标

- **不改 stdout 的原子性**。stdout 仍然是"跑完吐一个 JSON 对象"，一个字节不变。这是决策一的前提，不是妥协。
- **不给不带 `--ai` 的短命令加进度**。`tag apply`、`recipe apply` 单次都是毫秒到百毫秒级，加进度是纯噪音。
- **不做 agent 侧的进度条 UI**。Telegram 是文本消息流，播报形态就是一句话，不做消息编辑式的原地刷新（那是另一件事，会引入 message_id 状态管理）。
- **不动 `pzt open` 的 TUI 路径**。`/dedup --ai` 的 banner 原地重画在 T-14 已经做完且已真机验收，本增量不碰它。
- **不做 `Ingest`/`Deliver` 的进度**。它们的耗时是本地文件 IO，量级与 AI 阶段差两个数量级；真机踩到再说。
- **不把 curate 补成与 dedup 对称**。只补有消费者的两个进度回调，见决策七。
- **不统一 dedup 与 curate 的 scope/排除语义**。那是 T-16，独立条目：headless `cmd_dedup` 不做废片排除、交互 `/dedup` 做，同名操作语义已分叉。本增量不动那一层。

---

## 已拍板的设计决策

### 决策一：进度走 stderr，每行一个 JSON 对象（方案 A）

被否掉的两个备选：**B** 加显式 `--progress-fd` 开关（实现几乎相同，但多一个 flag 要解析、要文档化，而 `--json` 本来就已经是"调用方是 agent"的充分标记 - 所有 headless 命令都强制要求 `--json`，`cmd_dedup` 在 `!json` 时直接返回 usage 错误）；**C** 完全不碰契约、只做 (b)(c) 两条（进度降级成 agent 侧定时器播"还在跑"，是心跳不是进度，解决不了 U-10 的核心诉求"跑到哪了"）。

形状：

```
stderr: {"progress":{...}}    ← 运行期，0 到 N 行
stderr: {"progress":{...}}
stdout: {"groups":5,...}      ← 唯一一次，跑完才写
```

选 stderr 的三条依据：

1. **stdout 上多一行任何东西都会立刻炸**。`pzt_client.py:97` 是 `json.loads(stdout.strip())`。
2. **stderr 已经是结构化通道**。`commands.cpp:77` 失败时往 stderr 写一行 JSON 错误对象，这是既有约定。
3. **既有错误协议零改动**。`_parse_error`（`pzt_client.py:63-69`）取 stderr 的**最后一行**去 parse；命令失败时错误对象一定是最后写的，前面垫多少行进度都不影响它。命令成功时 `returncode == 0`，`_parse_error` 根本不会被调用。

契约表述相应更新为：**stdout 的原子性不变（一次提交、一次收尾、一个 JSON 对象）；stderr 是带外通道，承载运行期进度与失败时的错误对象，调用方可以完全忽略它而不影响正确性。**

已确认 stderr 在 headless 路径上是干净的：`DebugLogRedirect`（`cli/term/debug_log.h`）只在 `pzt open` 的 TUI 路径上接管 stderr，headless 命令不经过它。

### 决策二：进度行每次都写，节流在 agent 侧做

core 的回调每次都触发，cli 每次都往 stderr 写一行（本地管道，成本可忽略），**不在 C++ 侧做任何节流**。节流放在 agent 侧：`consumer` 按 `progress_interval_seconds` 决定要不要真的往 Telegram 发消息。

理由：Telegram 有速率限制，一个 20 张的簇会产生 19 次比较，每次发一条会被限流甚至封；但把节流做进 C++ 就等于让 core/cli 去猜下游的播报节奏，而下游不止 Telegram 一个（`run_watchfolder.py` 根本不播报）。谁播报谁节流。

连带收益：`--progress-interval-seconds` 的语义从"只管收图"自然扩成"所有周期性播报"，正好把 `agent/README.md` 那句不实描述改成真的（G6）。

### 决策三：~~`view.stage_progress` 保持 `(done, total)` 二元组，两级进度压成一级展示~~（**真机验收推翻，见下**）

原文：`SessionView.stage_progress` 的类型（`Optional[Tuple[int, int]]`）不变，`view.py:74-75` 那句"已完成 X/Y 张"的措辞形态也不变。AI 阶段的两级进度（组 + 比较）在到达 view 之前压成一级：**用比较次数**，因为那正是 `AiGateFn` 报给用户看的那把尺子。不引入新的 view 字段、不引入平行枚举 - `view.py:9` 已经写明"平行枚举只会漂移"。

**推翻的理由**：把 phase 压掉之后，展示层只剩一个写死的单位"张"，而各阶段数的根本不是同一种东西 - 本地分簇数**候选簇**、AI 阶段数**比较次数**、`StyleApplyAll` 才数**照片**。真机验收拿到的原话是"正在筛选，已完成 1/1 张"，用户要选的是 3 张，那个 1/1 其实是 1 个候选簇 - **单位和主语双错**，而且错得像 bug 而不像进度。

"不引入平行枚举"那条依据也用错了地方：`view.py:9` 说的是不要再造一个跟 `(status, current_stage, stage_progress)` 重复的步骤枚举，而"这个数字数的是什么"根本没有任何现存字段表达，不是重复，是缺失。

**修正后**：进度三元组 `(done, total, kind)`，`kind` 取 `orchestrator.stage.PROGRESS_{PHOTOS,GROUPS,COMPARISONS}`，从 stage 一路传到 view。传的是语义 key 不是"组"/"张"这种词 - prose 全部留在 `view.py` 的 `_PROGRESS_PHRASINGS`，`orchestrator` 与 `stages` 层不碰对话文本。

整句由 `kind` 决定，不是在 stage 的通用句子后面拼一个尾巴：数的东西不同，前半句也该不同。

| kind | 文案 |
|---|---|
| `groups` | 正在处理需要比较筛选的照片组，已完成 1/1组 |
| `comparisons` | 正在两两比较、挑出更好的那张，已完成 12/51次 |
| `photos` | 正在套滤镜，已完成 18/30张 |

"AI 阶段用比较次数当尺子"这半条决策仍然成立，被推翻的只是"压掉 phase"。

### 决策四：`StyleApplyAll` 的进度不经 stderr，纯 agent 侧产出

它是 Python 里的 for 循环，i/N 在循环里天然就有，不需要任何 C++ 配合。所以 `StageProgress` 事件有两个来源：跨进程解析出来的（dedup/curate）与进程内直接产生的（StyleApplyAll）。事件本身不区分来源。

这也意味着 G2 可以独立于 G1 先落地、独立验证。

### 决策五：`StyleApplyAll` 中途取消接受部分完成

取消发生在第 k 张时，前 k-1 张已经套上了 recipe。**不回滚**，理由：recipe 是 PZT 库内的状态、不碰原始文件（SPEC §2.4"不碰用户数据"），重跑会覆盖，部分完成无害。取消回执里说清"已经给 X/N 张套上了"，不假装什么都没发生。

这一条与 dedup/curate 的"取消一定是零写入"（`dedup.h:76-78`）**语义不同**，是有意的：那两个的写库统一在最后一步，天然零写入；`StyleApplyAll` 的写入本来就是逐张的，强行做成全有全无要引入事务边界，超出本增量范围且没有实际收益。

### 决策六：`ai_fallback_count` 进用户可见话术

不是只把字段带出来就完。Curate 结束时如果 `ai_fallback_count > 0`，播报里要说明有几组不是 AI 选的。措辞在 Eng Design 定，语义要求：用户能分辨"AI 帮我挑的"和"AI 没挑成、按时间选的"。

### 决策七：curate 只补有消费者的两个进度回调，不追求与 dedup 对称

两者共用同一个 `tournament::cluster_and_choose`，却在门面签名上差了整整一层。差的是这 4 + 2：

| | dedup | curate | 本增量 |
|---|---|---|---|
| `DedupProgressFn on_progress`（本地分簇进度） | 有 | 无 | **补** |
| `AiProgressFn on_ai_progress`（AI 两级进度） | 有 | 无 | **补** |
| `AiGateFn on_ai_gate`（开跑前开销闸门） | 有 | 无 | 不补 |
| `CancelFn on_cancel`（中途取消） | 有 | 无 | 不补 |
| 返回 `bool ai_declined` | 有 | 无 | 不补 |
| 返回 `bool cancelled` | 有 | 无 | 不补 |

**判据是有没有消费者，不是对不对称。**

补前两个，因为 G1 需要它们：Curate 是 Telegram 侧两个 AI 长阶段之一（`agent/stages/curate.py:45-46` 确实带 `--ai` 跑），`view.py:26` 那句"正在筛选..."之后的分钟级沉默正是 U-10 抱怨的一半。不补它们，Curate 在本增量收口之后仍然是纯黑箱。

不补后四个，因为它们今天没有、且可预见的将来也没有消费者：`/curate` 按 SPEC §3.2 已拍板不进 TUI（没人问闸门），agent 的取消是进程级的 `popen.terminate()`（不需要 `CancelFn` - dedup/curate 写库都在最后一步，SIGTERM 中途终止天然零写入）。

### 决策七的附注：一个潜伏的语义坍缩，本增量刻意不修

`tournament::ChooseSummary` 早就产出了 `ai_declined`/`cancelled`（`tournament.h:47-58`），但 `curate.cpp:110` 那句

```cpp
if (summary.clusters.empty()) return CurateResult{{}, count, 0, 0};
```

把它们直接丢弃了。而这两个 bool 为真时 `clusters` **恰恰就是空的** - 也就是说这段代码会把"用户取消了、零写入"和"候选池里一张都没有"折叠成完全相同的返回值。

**本增量不修它，是安全的**，前提正是决策七不补 `on_ai_gate`/`on_cancel`：两个钩子都不传，两个 bool 就恒为 `false`，折叠不可达。

**触发条件写在这里备查**：将来任何人给 curate 加上 `on_ai_gate` 或 `on_cancel`，必须在同一个改动里给 `CurateResult` 补上 `ai_declined`/`cancelled`，否则 `cmd_curate` 无法区分这两种 0。

### 不对称的根因（记档，供将来判断）

这不是设计取舍，是路径依赖冻出来的。四个回调各自的来历：

| 回调 | 引入提交 | 日期 | 驱动方 |
|---|---|---|---|
| `on_progress` | `6d77ae3` / `7bc4d09` | M3 | TUI `/dedup` 的进度条 |
| `AiGateFn` + `AiProgressFn` | `a1c7c02` | 2026-07-29 | T-14 那批（`667ecb5` 同日，`/dedup --ai` 控制台） |
| `CancelFn` | `e1f6a8b` | 2026-07-29 | T-9b（`/dedup` 中途 Ctrl-C） |

四个全部由 TUI `/dedup` 驱动，而 curate 没有 TUI 入口，从来没人走在会需要它们的路上。共享引擎 `cluster_and_choose`（`tournament.h:115-121`）四个参数一个不少，`dedup.cpp:183-186` 全部转发，`curate.cpp:104-107` 传到 `local_config` 就停了 - 所谓"差一整层"实际是**一个调用点少传四个实参**。

冻结的确切时刻有注释为证，`tournament.h:111-112`：

> `on_progress`：...**Commit 2 补上这个参数：`find_and_tag_duplicates` 今天的公开签名带这个回调，改调 `cluster_and_choose` 时不能悄悄把它吞掉。**

改造成共享引擎时确实做了"别把回调吞掉"这道检查，但判据是"`find_and_tag_duplicates` 的公开签名带不带它"。curate 的签名里本来就没有，没有东西需要保住，检查静默通过。**以"现有签名带不带"当判据，会让历史缺口自我复制** - 这是本条记档的主要价值。

---

## 待 Eng Design 解决的问题

这些不是需求层的取舍，是实现层的设计，列在这里是为了不被漏掉：

1. **stderr 进度行的具体 schema**（字段名、两级进度怎么表达、是否带 stage 标识）。
2. **决策七的落地面**。`core/curate/curate.h`、`curate.cpp`、`core/api.h`、`core/api.cpp` 四处加两个参数并转发；引擎侧零改动（`cluster_and_choose` 已经接得住）。`cmd_curate` 的 JSON 输出不变。
3. **`pzt_client` 的流式读改造，且不能踩回管道死锁**。这是本增量最大的实现风险，单列一节见下。

---

## 风险与已知限制

### 风险一（高）：流式读 stderr 可能踩回管道死锁

`_run_cancellable`（`pzt_client.py:99-116`）现在用 `communicate(timeout=)` 轮询，`:103-104` 的注释写明了选它的理由："比 poll()+PIPE 手工排水简单且不会管道死锁"。

问题是 `communicate` **不流式** - 超时时输出留在内部缓冲区，拿不到已到达的部分，只有子进程退出那一刻才一次性交出全部。也就是说今天这条路径读到 stderr 时命令早就跑完了，进度会变成事后回放，G1 落空。

要真流式就得自己排水，而这正是当初注释里点名要避开的坑：如果只盯着 stderr 读、不排 stdout，一个输出量大的命令会把 stdout 管道缓冲区填满、子进程阻塞在 write 上，父进程等在 stderr 上，双方互等。Eng Design 必须给出不死锁的读取方案（两个读取线程、或 selectors 多路复用），并且要有一条针对性的回归测试。

注意 `_real_runner`（`cancel_event is None` 的那条分支）不需要改：走那条路的都是不需要进度的短命令。

### 风险二（中）：curate 的进度总数在 AI 阶段之前是未知的

`AiGateFn` 报的 `comparison_count` 是分簇跑完之后才算得出来的。也就是说本地分簇阶段的进度分母（候选簇数）与 AI 阶段的分母（比较次数）是两把不同的尺子，中间会切换一次。播报措辞要能承受这个切换，不能让用户看到进度条"退回去"。

### 风险三（低）：`ai_fallback_count` 只在 `--ai` 时出现，不是"出现但恒为 0"

`commands.cpp:211,558` 都是 `if (ai_enabled) out["ai_fallback_count"] = ...`，这是 `W2026-07-21` 目标二刻意定的（不带 `--ai` 时 JSON 输出逐字节不变）。agent 侧读它必须用 `.get("ai_fallback_count", 0)` 而不是 `[...]`，否则不带 AI 的 Plan 会 KeyError。

### 已知限制：本增量不改变"进度是尽力而为"的性质

进度行丢失（管道满、agent 侧解析失败、行被截断）不应该让命令失败。agent 侧解析不出来的 stderr 行一律忽略，不告警、不重试 - 进度是观测，不是结果。这跟 T-10 预检"解析不出来就闭嘴"是同一条原则：为自己的格式假设过期而报错，等于把自己的 bug 报成用户的错。

---

## 验收标准

### 进度（G1/G2/G3）

1. `pzt dedup <项目> --scope '*' --ai --provider local --json` 在跑的过程中，stderr 上能看到逐行递增的进度 JSON，且 stdout 上仍然只有一个 JSON 对象、内容与本增量之前逐字节一致。
2. 同上，`pzt curate <项目> --count N --ai --provider local --json`。
3. 不带 `--ai` 时，本地分簇阶段的进度同样出现在 stderr 上（分簇本身在大批量下也是秒级以上）。
4. `_parse_error` 在"跑出过进度行然后失败"的命令上仍然能取出正确的错误对象（针对性单测）。
5. Telegram 真机：确认方案后进入 `RUNNING`，在 `progress_interval_seconds` 的节奏上收到带具体数字的进度消息，而不是一句"正在筛选..."之后的沉默。
6. `StyleApplyAll` 对 N ≥ 10 张的批次播报 i/N。
7. `SessionView.stage_progress` 在 `RUNNING` 期间被赋过非 None 值（不再是死字段）。

### 降级信号（G4）

8. `agent/stages/curate.py` 的 `StageOutput.data` 里带出 `ai_fallback_count`。
9. 构造一次"AI 比较必然失败"的 curate（provider 指向不可达地址），用户收到的完成播报里明确说明有几组不是 AI 选的。
10. 不带 `--ai` 的 Plan 走同一条路径不报错（风险三的 `.get` 兜底）。

### 取消（G5）

11. `Style` 阶段（`recipe suggest` 阻塞期间）用户说"停"，run 走 CANCELLED 收尾，不是 FAILED。
12. `StyleApplyAll` 跑到一半说"停"，run 走 CANCELLED，回执说清已经套了几张。
13. 已有的 Dedup/Curate 取消行为不回归（既有测试全绿）。

### curate 的进度钩子（G6）

14. `curate::curate` 与 `api::curate_images` 的签名含 `on_progress` 与 `on_ai_progress`，默认 `nullptr`，转发给 `cluster_and_choose`。
15. 现有全部 curate 调用点（`cmd_curate`、既有单测）零改动即可编译通过。
16. 两个回调各有一条 core 侧单测断言被调用且计数单调递增（SPEC §4.4）。
17. `CurateResult` 未变更；`on_ai_gate`/`on_cancel` 未加入签名（决策七的负向断言，防止实现时顺手加回来）。

### 文档（G7）

18. `agent/README.md` 对 `--progress-interval-seconds` 的描述与实际行为一致。
19. `docs/SPEC.md` §3.2 的契约表述更新为"stdout 原子 + stderr 带外"，并说明调用方可以完全忽略 stderr。
20. `docs/proposal-2026-07-25.md` 的 T-8 条目划掉并记录处置。

### 回归

17. `build` 与 `build_release` 双构建的 ctest 全绿。
18. agent pytest 全绿。
