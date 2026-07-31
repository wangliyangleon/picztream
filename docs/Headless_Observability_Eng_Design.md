# PZT 工程设计：headless 长运行的进度通道

- **对应 PRD**：`docs/Headless_Observability_PRD.md`（提案 T-8）
- **日期**：2026-07-31
- **范围**：只覆盖 PRD"待 Eng Design 解决的问题"里那三条。B（进程内进度）、C（降级信号）、D（取消补全）三段不需要设计决策，已按 PRD 直接实现并收口。

---

## 决策一：stderr 进度行的 schema

```
{"progress":{"phase":"cluster","done":3,"total":17}}
{"progress":{"phase":"compare","done":4,"total":51}}
```

一行一个 JSON 对象，`\n` 结尾，写完即刷（stderr 在 C 里本来就是无缓冲的，一次 `fprintf` 写完整行保证行原子）。

**外层包一个 `progress` key**，不是把 `phase/done/total` 平铺在顶层。理由是 stderr 上已经跑着另一种对象 - 失败时的 `{"error":..., "message":...}`（`commands.cpp:74-78`）。读取方要能一眼分辨，靠"有没有 `progress` 这个 key"比靠"有没有 `error` 这个 key"更稳（将来 stderr 上再多一种带外消息时，判据不用改）。

**`phase` 是必需的，因为分母会中途换尺子**（PRD 风险二）：本地分簇阶段数的是候选簇，AI 阶段数的是比较次数，两者不同源。不带 `phase` 的话读取方无法分辨"进度推进了"和"换阶段了"。

- `cluster`：`dedup::DedupProgressFn`，`(done, total)` 直接透传。
- `compare`：`dedup::AiProgress` 的 `comparison_done / comparison_total`。**两级压成一级，取比较次数**（PRD 决策三）：那正是 `AiGateFn` 报给用户看的同一把尺子（`dedup.h:52-58` 已论证过只报组号时单个大簇会静止几分钟）。`group_done/group_total` 不进 schema - 没有消费者，加了就是第二个"无人读的字段"。

**不做节流**（PRD 决策二）。本地管道写一行的成本可以忽略，节流是下游的事。

### 谁读、读到之后往哪送

`PztClient` 加一个 `progress_sink`，跟 `cancel_event` 同一个套路（挂在实例上，`call()` 内部零改动就能用）。stage 在调用前后挂上/摘掉：

```python
self.client.progress_sink = ctx.on_progress
try:
    result = self.client.call(*args)
finally:
    self.client.progress_sink = None
```

于是跨进程进度汇入 B.1a 已经打通的那条链路（`ctx.on_progress` → `Driver.progress_sink` → `StageProgress` 事件 → consumer 节流播报），worker 与 consumer **零改动**。

### 一个 run 里只转发一个 phase

`view.stage_progress` 是 `(done, total)` 二元组，没有 phase。两个 phase 都往上送的话，用户会看到"已完成 17/17 张"紧接着变成"已完成 1/51 张" - 分母和分子同时跳，读起来像进度条倒退（PRD 风险二要求避免）。

规则：**`ai_enabled` 时只转发 `compare`，否则只转发 `cluster`。** 依据是"哪个阶段是分钟级的"：开 AI 时耗时几乎全在比较上（每次一个受 60s 超时约束的视觉推理），分簇相对是一瞬；不开 AI 时压根没有比较阶段。

代价：开 AI 且批量很大时，分簇那一段仍然是静默的。接受 - 它比改造前的整段静默短得多，且 stderr 上两个 phase 都在，真要看有得看。

**stderr 上始终两个 phase 都写**，不因为 agent 只用一个就少写一个。CLI 的输出契约不该被当前唯一一个调用方的取舍绑架。

---

## 决策二：`core::curate::curate` 的参数补全

按 PRD 决策七只补两个有消费者的：

```cpp
CurateResult curate(db::Database& db, project::ProjectId project_id,
                     std::optional<tagging::TagId> candidate_scope, int count,
                     int time_window_seconds, int hash_threshold,
                     bool ai_enabled = false, ai::Provider ai_provider = ai::Provider::Local,
                     const ai::LocalModelConfig& local_config = ai::LocalModelConfig{},
                     dedup::DedupProgressFn on_progress = nullptr,          // 新增
                     tournament::AiProgressFn on_ai_progress = nullptr);    // 新增
```

追加在末尾、默认 `nullptr`，现有调用点零改动 - 跟 `W2026-07-21` 给 dedup 加这些参数时的做法一致。`core/api.h` / `api.cpp` 的门面同步透传。

**引擎侧零改动**：`tournament::cluster_and_choose` 四个参数一个不少（`tournament.h:115-121`），`curate.cpp:104-107` 现在传到 `local_config` 就停了，补上两个实参即可。

`on_ai_gate`/`on_cancel` 与 `CurateResult` 的 `ai_declined`/`cancelled` 不补，理由与触发条件见 PRD 决策七及其附注。

---

## 决策三：`pzt_client` 的流式读，且不踩管道死锁

### 现状为什么不流式

`_run_cancellable`（`pzt_client.py:99-116`）轮询 `communicate(timeout=)`。`communicate` 超时时输出留在内部缓冲区，拿不到已到达的部分，只有子进程退出那一刻才一次性交出全部。`:103-104` 的注释写明了当初选它的理由："比 poll()+PIPE 手工排水简单且不会管道死锁"。

那条理由今天依然成立，所以改造不能简单地换成"只盯 stderr 读"：stdout 管道被填满时子进程会阻塞在 write 上，父进程等在 stderr 上，双方互等。

### 方案：两个读取线程 + 主线程只做取消轮询

```
reader-stdout ──┐
                ├─→ 各自读到 EOF 为止，全程不阻塞对方
reader-stderr ──┘   （stderr 每读到一行就立刻回调 progress_sink）

主线程：轮询 cancel_event，命中就 terminate/kill，然后 join 两个 reader
```

选它而不是 `selectors` 多路复用的理由：两条管道各读各的，逻辑上没有交织，用线程写出来就是两个五行的 `for line in pipe`；`selectors` 要手工处理半行缓冲、EOF 注销、还要跟取消轮询挤在同一个循环里。这条路径不是性能敏感的（一次子进程调用两条线程，量级是分钟级任务里的微秒），可读性优先。

**排水必须无条件**：即使调用方没挂 `progress_sink`，stderr 的 reader 也照读不误（只是不回调），否则进度行会把管道填满、把子进程卡死。这是本次改造最容易写错的地方，要有针对性回归测试 - **构造一个 stderr 输出量远超管道缓冲区（macOS 上 64KB）的假子进程，断言它能正常跑完而不是超时**。

**`_real_runner` 那条分支不动**（`cancel_event is None`）：走它的都是不需要进度的短命令，`subprocess.run` 内部本来就用 `communicate`，没有死锁风险。

### 解析规则：尽力而为

- 不以 `{` 开头、或 JSON 解析失败、或没有 `progress` key 的行：**忽略**，不告警不重试。
- `progress` 里字段缺失或类型不对：**忽略这一行**。
- 全部 stderr 行仍然原样累积返回，`_parse_error` 取最后一行的既有约定不变（命令失败时错误对象一定是最后写的，前面垫多少进度行都不影响）。

依据同 T-10 的预检：为自己的格式假设过期而报错，等于把自己的 bug 报成用户的错。进度是观测，不是结果。

---

## 实现顺序

| | 内容 | 层 |
|---|---|---|
| A.1 | `cmd_dedup` 按决策一往 stderr 写进度行（同时定下 schema） | cli |
| A.2 | `curate::curate` / `api::curate_images` 按决策二补两个参数 | core |
| A.3 | `cmd_curate` 接上同一套写出 | cli |
| A.4 | `pzt_client` 按决策三改成流式读 + 防死锁回归测试 | agent |
| A.5 | `DedupStage`/`CurateStage` 按决策一挂 `progress_sink`，只转发一个 phase | agent |

A.1 先行是因为它定 schema；A.4 是风险最高的一段，独立成一个可提交单元。
