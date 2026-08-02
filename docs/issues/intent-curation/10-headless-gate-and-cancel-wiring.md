# 10 - headless 的开销闸门与取消接线

**What to build:** 让 PRD 的 G5（用户可以在 AI 开跑前拒绝）在真机上端到端可达。今天它只在 core 里可达。

票 05 把开销闸门、取消、以及 `ai_declined`/`cancelled` 两个返回字段都做进了 `core/curate`，也有测试覆盖。但 headless 这一侧没接：

- `cli/commands/commands.cpp` 的 `cmd_curate` 给 `on_ai_gate` 传的是 `nullptr`，`on_cancel` 取默认值 `nullptr`。`pzt dedup --ai` 的 headless 路径（`commands.cpp:586`）同一处置 - 原以为 `pzt open` 控制台那个 TUI 闸门算兜底，但 agent 走的正是 headless，兜不到，所以两条命令一起进本票范围（拍板决策五）。
- `CurateResult.ai_declined`/`cancelled` 没有序列化进 `pzt curate` 的 JSON 输出。票 05 的落地记录写明了当时不加的理由：两个钩子都是 `nullptr` 时这两个字段恒为 false，加进去是死字段。

**后果**：真机上 `pzt curate --ai` 不会问任何人就开始花钱，也不能中途叫停。agent 那一侧无从得知用户拒绝过。

**语义约束（票 05 已定，本票不得推翻）**：取消**不是零写入**。评估逐张写库，喊停时已评估完的那几张留在库里，这是有意的 - 每条记录本身完整，留着正好被下次运行的缓存判据命中。用户话术必须如实反映这一点。

## 拍板（2026-08-02）

### 卡住这一票的是一条真矛盾，不是接线偷懒

PRD 决策九写死"整条流程封在一次 `pzt curate` 调用里"，G5 要求"在精确开销算出来那一刻让用户拒绝"。精确开销只有分簇跑完才知道，那一刻进程正阻塞在 core 里，而 Telegram 那头的用户不在同一个时间轴上。**同步阻塞式闸门 + 单次调用 + 异步聊天界面，三个只能取两个。** 原文列的两条候选各自放弃了一个：agent 自算放弃精确，阈值参数放弃"用户点头"。

真机上这条路径今天长这样（40 张照片、"选三张发朋友圈"、点了 AI 筛选）：

```
Dedup  --ai   分簇 → 锦标赛比较        ~12 次视觉调用
Curate --ai   再分簇 → 预选集 6 张     ~6  次视觉调用
              → 1 次纯文本选择调用
```

本地模型一次约 40 秒，合计七八分钟，全程没人问过用户。

### 决策一：headless 上的闸门从「阻塞式确认」改成「告知 + 随时可撤」

core 在闸门那一刻**不等**任何人，把精确开销（比较次数 + 评估张数）经 stderr 带外通道吐一行，然后直接继续跑。agent 收到后立刻发给用户并给可取消入口；用户点取消走**已有的** kill 通路（`worker.KILLABLE_STAGES` 里已经有 `"Curate"`）。

一次调用、数字精确、用户真能拒绝、不往返 - 决策九不受触碰。

被否掉的三条与理由：

- *agent 自己算一遍开销，挂在已有的方案确认上*：比较次数要分簇之后才知道、评估张数要扣缓存，agent 只能报区间。它作为 C 的补充仍然有价值（一次调用都没发时就先说话），但**不在本票范围**，想做单开票。
- *`--ai-budget` 阈值参数*：阈值由 agent 定死，用户从未被问过。它是**保险丝**不是闸门，防的是意外跑一个超大批量，自己不满足 G5。可以晚点独立加。
- *维持现状*：G5 在 Telegram 上不可达，用户在七八分钟里对花销一无所知。

### 决策二：放弃「拒绝时零写入」，与票 05 已定的取消语义并轨

这是决策一的直接代价，必须明写：用户看到开销数字时，评估可能已经开跑了几张。票 05 已经为取消定了同一套语义（评估记录留库是有意的，正好被下次缓存命中，回滚等于下次再花一次钱）。本票只是把闸门也纳进这套语义，不是新开一个例外。

**因此 PRD 的 G5 与决策十八的措辞必须在本票内改**，不是可延后的文档整理：阻塞式零写入闸门的承诺收窄到有 TUI 的入口（`pzt open` 里的 `/dedup --ai`，`browse.cpp:427`，已实现且不动），headless 侧改述为「事前告知精确开销 + 事中进度 + 随时可取消」。

### 决策三：`ai_declined`/`cancelled` 在 headless 上仍然不序列化

决策一之后 headless 不再有返回 false 的 gate，取消又是 SIGTERM（进程被杀、没有 stdout），所以这两个字段在 headless 上**依旧恒为 false**，加进 JSON 依旧是死字段。票 05 落地记录里"等接线时一起补"的那句到此作废 - 接线的形状变了，补的前提不成立。

两个字段在 core 内部仍然正确、仍被测试覆盖，服务的是 TUI 与未来的非 headless 调用方。决策十九的债是在 core 层还的，本票不改这个判断。原验收标准里那一条因此删除。

### 决策四：取消只补话术，不接 core 的 `on_cancel`

机制不缺 - `Curate` 已在 `KILLABLE_STAGES` 里，`pzt_client._run_cancellable` 会 SIGTERM 掉子进程，**中途取消今天就能停**。缺的是诚实：`PARTIAL_ON_CANCEL_STAGES` 只有 `StyleApplyAll`，用户被告知的只有"已取消"，没人说已评估完的那几张留在库里、下次不用重花钱。

不接 `on_cancel`（优雅停 + 正常返回 `cancelled=true`）的理由：SIGTERM 已经能停，多这一层只买到"已评估张数由 core 说而不是从 stderr 进度推断"，而进度行本来就逐张吐、`worker._last_progress` 已经存着这个数。

### 决策五：`pzt dedup --ai` 的 headless 路径同刀做，不单开票

真机场景里 agent 的 Dedup stage 先烧掉十几次调用，Curate 才报数 - 只做 curate 的话，用户看到"18 次"时前面那笔已经花掉了，G5 在这条路径上只覆盖了后半段。**开销这件事对用户是一笔账，不是两笔**，跟决策十七"用户心智里没有簇内簇外的区别"是同一个立场。

（拍板时曾倾向单开一票，理由是 dedup 有 `pzt open` 的阻塞闸门兜底、缺口没 curate 那么裸。被推翻：那个兜底只在 TUI 上，agent 走的是 headless，兜不到。）

**必须拆清楚的两条 dedup 路径**：

- `pzt open` 控制台里的 `/dedup --ai`（`browse.cpp:427`）：阻塞式闸门 + 零写入承诺，**一个字都不动**。
- `pzt dedup --ai --json` headless（agent 的 Dedup stage 调的那条，`commands.cpp:586` 传 `nullptr` 的地方）：加 cost 事件，与 curate 同一处置。`Dedup` 也早就在 `KILLABLE_STAGES` 里，取消通路现成。

连带影响：决策二要改的 PRD 措辞现在同时牵扯 dedup 既有的零写入承诺 - 改述必须按**入口**（TUI / headless）切，不能按**命令**（dedup / curate）切，否则会把控制台那条已经实现且正确的闸门一起改错。

### 开工前仍需回答（Eng Design 级，不阻塞拍板）

1. **cost 事件在 stderr 上的形状**。T-8 既定 schema 是 `(phase, done, total)`，而开销不是"完成了几分之几"。是复用一个 `kind` 把两个数塞进 done/total，还是在同一条通道上开一种新事件类型？后者要动 `stages/progress.py` 的解析。
2. **两条命令的 cost 事件在 agent 侧怎么合成一句话**。Dedup 与 Curate 是两个 stage、两次子进程调用，各报各的。是各发一条消息（用户看到两次"要跑 N 次"），还是攒起来？攒不了 - Curate 的数字要等 Dedup 跑完才知道。倾向各发各的，措辞上让第二条读起来是接续而不是重复。

**Blocked by:** 06（模型选择与排序接进 curate）

**Status:** done（2026-08-02，Telegram 端到端真机验收通过）

- [x] 真机上 `pzt curate --ai` 在开始逐张评估之前，用户收到一条带**精确**开销（比较次数 + 评估张数）与预计时长的消息
- [x] 真机上 `pzt dedup --ai` 的 headless 路径同样报出精确开销，且用户在 Dedup 烧掉第一次调用之前就收到
- [x] 两条消息各自带可取消入口，点了能真的停下（`Dedup`/`Curate` 都已在 `KILLABLE_STAGES` 里）
- [x] 第二条消息读起来是接续而不是重复第一条
- [x] 取消话术如实说明已评估/已比较的那部分留在库里，不谎称零写入
- [x] 每条命令的开销消息只发一次（core 侧 `gate_consulted` 的两个触发点合起来只报一次，行为不变）
- [x] `pzt open` 里 `/dedup --ai` 的阻塞式闸门与其零写入承诺**一个字不动**，真机复核过
- [x] 关 AI 时两条命令都不产生 cost 事件
- [x] PRD 的 G5 与决策十八改述为按**入口**（TUI / headless）切，不按命令切，且仍覆盖 TUI 那条阻塞闸门（`3df88ce`，见 `docs/Intent_Curation_PRD.md:67`）
