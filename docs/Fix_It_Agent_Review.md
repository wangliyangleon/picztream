# PZT Agent 层三视角评审 Fix-it(2026-07-17)

> 针对 M4 增量一 + W2026-07-15 AgentRuntime 重构后的 `agent/` 全量代码(基线 commit `14bee68`,252 条测试全绿)的一次专项评审。三个视角:**用户**(模拟 Telegram 端完整工作流与非常规输入)、**架构**(模块与流程设计的漏洞/过度设计/扩展性)、**工程**(代码级 bug/性能/冗余)。评级定义沿用 `docs/Fix_It_Night_Review.md`(难度 S/M/L,复杂度 低/中/高,优先级 P0-P3,类别与来源视角同款),编号用 `AG-` 前缀与 CLI/core 的 `F-` 区分。

## 总体评价(先说结论)

* **架构层面是健康的**:consumer/worker 双 lane 拆分、"落盘 + 事件"所有权交接、generation 防陈旧回调、按钮回调作为唯一确定性路径,这些设计与问题规模匹配,没有发现明显的过度设计;测试面(consumer 流/按钮/timers/worker/取消)扎实。
* 但**发现一组集中在"Style 闸门"和"事件协议"上的真实 bug**:最严重的一类是用户在风格闸门处的正常回复("算了不弄了"/"不要滤镜")可以让整批(含几分钟 Evaluate 结果)直接报废(AG-01/AG-02);其次是 JobCrashed 事件不区分 lane 导致的误报与状态污染(AG-03)。这些都是双 lane/多分类器几轮真机迭代叠加后留下的接缝问题,单条修复量都不大。
* 用户视角还产出一批扩展提案(照片 caption 当意图、/命令快路径、预览编号等),集中列在 AG-16 与各 P2 条目。

未发现 P0(happy path 已真机验证可用,无与已验收 PRD 直接矛盾项)。

## 逐项清单

### P1

---

**AG-01 风格匹配失败把整批打成 FAILED,不可恢复** ✅ 已修复
类别: Bug | 来源视角: 用户 + 工程

现象: Style 闸门问"想要什么风格?"后,用户的描述若无法映射到 preset(`compose/style_matcher.py:61` 抛 `hallucinated`,本地 gemma 小模型下并不罕见;"不要滤镜"/"原图就行"这类合理诉求则必然失败),`stages/style.py:41` 返回 `ok=False`,而 Style 是 `criticality="critical"`(`stages/style.py:21`),`driver._run_stage` 直接把 run 置 FAILED → worker 发 `RunFinished(FAILED)` → consumer 报"处理失败"并清会话。FAILED 是终态,几分钟的 Evaluate/Dedup/Curate 结果全部报废,照片留在旧 run 的 incoming 目录里无法被新批次复用。

修法: 风格决策失败不该是 stage 级失败。`StyleMatchError`/空描述应降级为"重新回到 Style 闸门提问"(比如 StyleStage 对匹配失败返回 `ok=True, data={"chosen_recipe": None}`,worker 在 `_report_stop` 看到 gate 停在 Style 且无 recipe 时重新发 `GateReached(Style)`,consumer 已有"没能选出风格,直接说说想要什么风格吧"文案可复用);同时补"不要风格/跳过"这条产品路径(见 AG-02、AG-16)。

难度 M | 复杂度 中 | 优先级 P1

修复记录(2026-07-18, "Style 闸门健壮化"增量): StyleStage 匹配失败改返回 `ok=True {chosen_recipe:None, match_failed:True}`(不再 critical 报废); worker `_execute_drive` 的 rerun_style 见到 match_failed → `driver.rearm_gate("Style")` + 重发 `GateReached(Style, {match_failed:True})`, consumer 原地重问"没能选出对应的风格,换个说法..."。空描述改为 skip 空跑(见 AG-16.1)。与 AG-02 同片代码一起做。

---

**AG-02 Style 问描述闸门对文本零分类:取消/提问会被当成风格描述** ✅ 已修复
类别: Bug + 交互打磨 | 来源视角: 用户

现象: `session/consumer.py:318` 在 gate==Style 时任何文本直接走 `rerun_style`,不经过任何分类器。这是"彻底去文本精确匹配、取消意图全交 LLM"(Eng Design 真机反馈第四/五轮)改造后**唯一漏掉的状态**:用户在这里说"算了不弄了"会被当风格描述丢给 style_matcher,轻则匹配出一个莫名风格,重则触发 AG-01 直接炸批;"有哪些风格?"这类提问同样不可达。与其它所有状态"自然语言取消 + 二次确认"的行为不一致。

修法: Style 问描述处也过一个小分类器(describe/cancel/query/skip 四类,与 `classify_style_gate_reply` 同款紧 schema),describe 才走 rerun_style;skip 对应 AG-01 的"不要风格"路径。分类失败降级为现状(当描述)。

难度 S | 复杂度 低 | 优先级 P1(与 AG-01 一起修)

修复记录(2026-07-18): 新增 `classify_style_describe`(describe/skip/cancel/query 紧 schema)+ `StyleDescribeReply`; consumer `_process_text` gate==Style 改为过 `style_describe` 分类,新增 `_on_style_describe_reply`(cancel→二次确认 / query→列 preset / skip→原图直出 / describe→rerun_style); worker `_execute_classify` + `run_telegram` 接线; 分类失败降级为当描述。

---

**AG-03 JobCrashed 事件不区分 lane:classify 崩溃会污染 drive 会话状态** ✅ 已修复
类别: Bug + 健壮性 | 来源视角: 工程 + 架构

现象: `session/protocol.py::JobCrashed` 只有 generation + error,没有 job 类型;`consumer._on_job_crashed`(consumer.py:661)用 `was_driving = self.view.drive_active` 猜崩的是谁。drive 正在跑 Evaluate 时,用户一句"到哪了"的 classify job 若意外崩溃(分类器抛非预期异常),consumer 会:① 误报"处理过程中出了点问题,这批先停在这儿了"(drive 其实好好的);② 把 `drive_active`/`active_drive_job` 清掉,于是下一条任意消息触发 `_enqueue_drive("resume")`,与仍在跑的原 DriveJob 在 drive 队列里排成两个,真闸门到达后被 resume job 重复触发一遍(预览媒体重发、闸门重复提问);③ 此窗口内用户取消会落入 `_do_cancel` 的"无主 RUNNING"分支,consumer 直接 load+cancel 落盘,与 worker 持有的内存副本形成写写冲突,worker 下一次 save 会把 CANCELLED 悄悄改回 RUNNING(取消被静默吞掉)。

修法: `JobCrashed` 加 `lane`(或 `job_kind`)字段;classify 崩溃只清 `inflight` 并回"刚才那条没能处理",不碰 drive 相关状态。顺带处理对称情形:drive 崩溃时若有 classify inflight,不要连带清掉(那条文本会没有任何回复)。

难度 S | 复杂度 中 | 优先级 P1

修复记录(2026-07-18): `JobCrashed` 增 `lane` 字段(`protocol.py`),worker 从 job 类型判定 lane 随事件带出(`worker.py::_step`),consumer `_on_job_crashed` 按 lane 分流只清对应 lane 状态(`consumer.py`)。补 worker「drive 崩发 lane=drive」与 consumer「classify 崩不碰 drive / drive 崩不清 inflight」两向测试,全量 255 条绿。

---

**AG-04 Deliver 闸门做任何调整都会重新问一遍风格** ✅ 已修复
类别: Bug + 交互打磨 | 来源视角: 用户

现象: 在 Deliver 闸门说"换掉第3张"/"留5张",`apply_adjustment` 会把 Curate 的全部下游(Style/StyleApplyAll/Deliver,`driver._downstream_of`)重置为 PENDING;推进时 Style 的 `gate="required"` 且 `gate_state` 已清空,于是又停在 Style 闸门问"想要什么风格?",尽管 `style_description`/上次选中的 recipe 还在 spec.params 里。用户换一张照片要把风格、预览确认、交付确认三道闸门全部重走。这是 W2026-07-15 在 M4 原管线(无 Style)中间插入两个带闸门 stage 后的回归,现有测试只断言到 adjustment job 入队,没盖到之后的闸门序列。

修法: 已经有答案的闸门不再问。最小改法在 consumer:`_on_gate_reached(Style)` 时查 run 的 Style spec,若 `style_description` 非空则直接回一句"还按「X」重新套用"并自动 `_enqueue_drive("rerun_style", 原描述)`(用户想换仍可在 StyleApplyAll 预览闸门重选)。StyleApplyAll 预览闸门保留(选片变了预览确实该重看)。

难度 M | 复杂度 中 | 优先级 P1

修复记录(2026-07-18): consumer `_on_gate_reached(Style)` 在非 match_failed 分支查 Style spec.params——`style_description` key 存在(只有答过才有)即不重问,`prev` 非空回"还按「X」重新套用"、为空回"还是不套滤镜",都自动 `_enqueue_drive("rerun_style", prev)`。首次到达(无该 key)仍照常问。consumer 单文件改动 + 两条测试,全量 271 条绿。

---

**AG-05 带闸门 stage 的 StageStarted 误发:"正在交付..."紧跟着交付确认闸门** ✅ 已修复
类别: Bug | 来源视角: 用户 + 工程

现象: `session/worker.py:209` 在 `advance()` 之前按 `peek_next_stage()` 发 StageStarted,但 advance 对带闸门的 stage 会停在闸门而不运行它。Deliver 的闸门是每批必挂的(consumer 注入 `gate="required"`),于是**每一批**用户都会看到"正在交付..."紧跟"选好了 N 张,满意点满意"的自相矛盾序列(test_worker.py::test_full_gate_walk 的事件序列可复现);而用户批准后 `resolve_gate` 直接运行 stage,真正交付时反而没有任何进度提示。Style 也同样被误发,只是恰好不在文案表里、只污染 `view.current_stage`。

修法: worker 在发 StageStarted 前判断"这一步会真的运行还是会停闸门"(`peek` 返回 spec,`spec.gate != "off" and run.gate_state is None` 时不发);`resolve_gate`/`rerun_style` 路径在实际运行前补发对应 StageStarted,让"正在交付..."出现在批准之后。

难度 S | 复杂度 低 | 优先级 P1

修复记录(2026-07-18): driver 新增无副作用 `peek_next_spec`(`peek_next_stage` 改为其薄壳);worker `_drive_to_stop` 仅为"这一轮真会运行"的 stage 发 StageStarted(停闸门判据同 `driver.advance`),`_execute_drive` 在 `resolve_gate`/`rerun_style` 调 driver 前补发被闸门挡住的那个 stage 的 StageStarted。更新 gate-walk 测试断言 StageStarted 时序、修正 `test_drive_start_runs_to_style_gate`(不再误发 Style)、加聚焦测试,全量 256 条绿。

---

**AG-06 交付导出失败被 optional 语义吞掉,收尾却说"这批就处理完啦"** ✅ 已修复
类别: Bug | 来源视角: 用户 + 架构

现象: DeliverStage 是 `criticality="optional"`(`stages/deliver.py:25`,本意是"交付失败不该抹掉选片结果")。export-images 失败时返回 `ok=False` → 被记为 SKIPPED → run 继续推进到 AWAITING_REVIEW → 自动 approve → `RunFinished(DONE)` → consumer 发"这批就处理完啦～想开新的一批随时发照片"(consumer.py:657)。用户一张照片都没收到,却被明确告知全部完成。旧 router 时代 DONE 不发收尾消息,这个误导是"一批开始与结束都有反馈"改造后新引入的。

修法: `RunFinished(DONE)` 的 detail 里带上 SKIPPED 的 critical-path stage(worker `_report_stop` 检查 `run.stage_states` 里 Deliver 是否 SKIPPED),consumer 对"完成但交付被跳过"改发"处理完了,但交付失败:{error},说'重试'我再发一次"并保留 run 可 resume;或者干脆把 Deliver 在闸门确认后的这次执行视为 critical(用户已经明确说了"满意",交付失败就是失败)。两案取一,倾向后者(语义更直)。

难度 S | 复杂度 中 | 优先级 P1

修复记录(2026-07-18, 采用方案 B): `DeliverStage.criticality` 改为 `"critical"`(deliver.py, 唯一生产改动)。交付失败 -> run FAILED -> worker `_report_stop` 既有逻辑发带 detail 的 `RunFinished(FAILED)` -> consumer 既有 FAILED 分支发"处理失败:Deliver:{error}", worker/consumer 零改动。选片结果仍持久化在 run.outputs 不丢; watchfolder 自动模式下交付失败同样 FAILED。FakeClient 加 `raise_command_on` 参数, 补 worker「export 失败 -> run FAILED」与 stage「Deliver is critical」测试, 全量 258 条绿。

---

### P2

---

**AG-07 inline 按钮绕过文本 FIFO 串行保护,旧参数可能抢跑**
类别: Bug | 来源视角: 工程

现象: 文本串行规则专门防"改成30张"还没解析完、"好的"抢跑(consumer 顶部 docstring),但按钮回调是即时路径、不检查 `inflight`:用户打字"改成6张"(refine classify 在途)后立刻点方案消息上的"好的"按钮,`_handle_callback` → `_begin_running` 用旧参数开跑;refine 结果回来时 run 已易主,被 `_on_refine_reply` 的状态检查静默丢弃。闸门处"打字调整 + 点满意"同理。

修法: `_handle_callback` 在 `inflight is not None` 时回"上一条还在处理,稍等一下再点"(或把回调排进 pending_texts 队尾语义)。

难度 S | 复杂度 低 | 优先级 P2

---

**AG-08 零照片也能组方案开跑;第二句意图会整句覆盖草稿** ✅ 已修复
类别: Bug + 交互打磨 | 来源视角: 用户

现象: 草稿态(意图先到、照片未到)下用户再说一句意图,`_on_collecting_reply` 的 intent 分支(consumer.py:441)不查 `photo_count` 直接 `_submit_compose(text)`:① 0 张照片也能走到 PLANNED、被确认后 `pzt new` 对空目录失败,整个流程以"处理失败:Ingest:..."收场(idle 自动组方案路径反而有 `count > 0` 保护,两条路径不一致);② compose 只用最新一句文本,"选三张发朋友圈"的草稿被"标签叫ins吧"整句覆盖,count 退回默认 9,用户先前的约束静默丢失。

修法: ① intent 分支在 `photo_count == 0` 时并入草稿(更新 `intent_raw`、回"记下了,等照片");② 已有草稿时把 `intent_raw + 新句子`拼接后再 compose(compose_plan 的 prompt 天然能消化多句意图)。

难度 S | 复杂度 低 | 优先级 P2

修复记录(2026-07-18): consumer 新增 `_merge_intent` helper; `_on_collecting_reply` 的 COLLECTING intent 分支按 `photo_count()==0` 分流(0 张并入草稿不 compose, 有照片则拼接草稿+新句再 compose); start 分支补 0 照片守卫(回"还没收到照片哦")。consumer 单文件 + 三条测试, 全量 274 条绿。

---

**AG-09 照片 caption 里的意图被静默丢弃** ✅ 已修复
类别: 交互打磨 + 需求proposal | 来源视角: 用户

现象: Telegram 用户最自然的用法是发图时把"帮我选3张发朋友圈"写在照片 caption 里(尤其相册多选一次发)。`transport/telegram.py::_handle_update` 的 photo/document 分支只取文件,caption 被丢弃(只在"不认识的消息形状"诊断打印里出现过,telegram.py:111)。用户视角看就是"说了话没人理",还得再打一遍字。

修法: photo/document 分支读 `message.caption`,非空时在照片入站消息之后追加一条同 chat 的 text 入站消息(复用现有文本管线,不需要新 kind);相册多张只有第一张带 caption,天然只触发一次。

难度 S | 复杂度 低 | 优先级 P2

修复记录(2026-07-18): transport 新增 `_enqueue_caption(message)`, photo/document 分支各调一次(排在照片/文件消息之后)。复用现有 text 管线, 无新 kind。加两条 transport 测试(photo/document 带 caption), 全量 276 条绿。

---

**AG-10 `ClassifyFailed.retryable` 无人消费:闸门/refine 处的基础设施故障被说成"没听懂这句话"**
类别: Bug + 纯清理 | 来源视角: 工程

现象: 真机反馈明确要求区分"AI 连不上"和"没看懂"(Eng Design 刻意偏离清单第 4 条),但只有 compose 路径做了(`_looks_like_infra_error`)。worker 忠实地把 `LlmRequestError → retryable=True` 编码进 `ClassifyFailed`,而 `consumer._on_classify_failed` 从头到尾没读过 `event.retryable`:Ollama 挂掉时,gate_reply/refine/style_gate 的用户会被反复告知"没听懂这句话,能再说清楚点吗",把基础设施故障误导成表达问题,正是当初要修的体验。

修法: `_on_classify_failed` 对 `retryable=True` 统一先回"AI 服务好像连不上,稍后再试";`retryable=False` 才走各 kind 的"没听懂"分支。字段既然有了就用上,不用就删。

难度 S | 复杂度 低 | 优先级 P2

---

**AG-11 consumer 单轮异常会丢掉同批入站消息和已出队事件**
类别: 健壮性 | 来源视角: 工程

现象: `run_telegram.py` 主循环把 `consumer.step()` 整体 try 住,但 `step()` 内部 `transport.receive()` 已把队列整批取成 list,处理第一条时若 `_send` 抛异常(Telegram 超时是真机常见故障,`future.result(timeout=30)`),剩余几条入站消息随异常整批丢失;`_drain_events` 同理,事件出队后处理中途异常即丢(比如 GateReached 已 load run、发闸门文案失败,用户永远等不到那句提问,只能靠 300s idle 提醒兜底)。

修法: `_handle_inbound`/`_apply_event` 各自 per-item try/except(打印 + 跳过该条,不吞掉整批);`_send` 对 transport 异常做一次退避重试后放弃。不追求消息不丢的强保证(聊天场景不值得),只把故障半径从"整批"缩到"单条"。

难度 M | 复杂度 中 | 优先级 P2

---

**AG-12 取消/崩溃竞态可留下双活跃 run,bootstrap 的 assert 直接拒绝启动**
类别: 健壮性 | 来源视角: 架构

现象: drive 期取消走"置 cancel_event + 立即 `_reset_session`",盘上旧 run 要等 worker 收尾才变 CANCELLED;此窗口内用户发照片会 mint 新 COLLECTING run,瞬时存在两个非终态 run。正常情况下 worker 几秒内收尾,但若 worker 恰在此期间崩溃(或进程被杀),旧 run 永久停在 RUNNING:下次启动 `bootstrap` 的 `assert len(active) <= 1`(consumer.py:114)直接 AssertionError,常驻进程起不来,需要手工删 JSON 才能恢复;即使只有旧 run 一个,bootstrap 也会把用户明确取消过的批次当"上次被中断"自动复活续跑。

修法: bootstrap 把 assert 换成自愈:多个活跃 run 时保留 `last_activity_at` 最新的一个,其余直接 `driver.cancel` 落盘并打印;取消路径在 `_do_cancel` 时把 `cancelling_run_id` 落一个小标记文件,bootstrap 看到即不复活、直接补 cancel。

难度 S | 复杂度 中 | 优先级 P2

---

**AG-13 语言推理 provider 与 Ollama 模型名硬编码,入口无开关**
类别: 工程规范 + 交互打磨 | 来源视角: 架构

现象: SPEC 3.3 说视觉/语言 provider 均可插拔,但 run_telegram 运行时里所有分类器/compose 调用都吃默认 `meta_provider="local"`(worker 调用处无一传参),`_OLLAMA_MODEL="gemma4:e2b"`/`_CLAUDE_MODEL` 也是 `compose/llm_client.py:16-24` 的模块常量。换模型或临时切云端(本地模型分类质量不够时的现实需求)要改代码。函数签名层可插拔、wiring 层写死,插拔是名义上的。

修法: `PZT_AGENT_META_PROVIDER`/`PZT_AGENT_OLLAMA_MODEL` 环境变量(或 run_telegram 命令行 flag),`build_runtime` 读一次、经 functools.partial 注入各分类函数,worker 零改动。

难度 S | 复杂度 低 | 优先级 P2

---

**AG-14 agent 状态目录无限增长,无任何清理/保留策略**
类别: 健壮性 + 性能/内存 | 来源视角: 架构

现象: `~/.pzt-agent` 下 runs/(每批一个 JSON,终态永不删)、incoming/(每批全量原图拷贝)、staging/、preview/、deliver-out/、telegram-inbox/(下载的原始副本,入 incoming 前还 copy2 了一份,同一张图至少落盘两份)全部只增不减;core 侧每个 run 还建一个 pzt 项目(project_id=run_id)同样无人回收。常驻家用机按月跑,磁盘占用线性膨胀,`list_active()` 启动时也要 load 全部历史 JSON。

修法: 终态 run 的收尾钩子(RunFinished 应用时)删除该 run 的 incoming/staging/preview 与 telegram-inbox 源文件,保留 deliver-out 与 run JSON;再加一个启动时的"终态超过 N 天清 JSON + `pzt delete` 对应项目"的低频清扫。保留策略(比如 DONE 保留 7 天以便"重发一次")需要一次小拍板。

难度 M | 复杂度 中 | 优先级 P2

---

**AG-15 预览图无编号,"换掉第3张"全凭用户数图**
类别: 交互打磨 | 来源视角: 用户

现象: Deliver 闸门的调整语法以"第 N 张"为锚(1-based,对应 Curate selected 顺序),但 `worker._send_preview_media` 逐张裸发无 caption;某张发送失败(计入 failed_count)时用户看到的序列还会与真实下标错位,"换掉第3张"换错张。另外每次调整后闸门重发全部预览(9 张批就是 9 张重传),只换一张也全量刷屏。

修法: `send_photo` 支持 caption(telegram_client 一行),预览逐张带"第 N 张";发送失败的那张退化为发"第 N 张预览发送失败"文本占位,保序。全量重发可后续再优化(只重发变化的张,需要 diff 上次 selected,可等真机反馈)。

难度 S | 复杂度 低 | 优先级 P2

---

### P3

---

**AG-16 扩展提案打包(用户模拟视角产出,均需产品拍板)**
类别: 需求proposal | 来源视角: 用户

按预期价值排序:
1. **"不要滤镜/原图直出"路径** ✅ 已随"Style 闸门健壮化"增量落地(2026-07-18):与 AG-01/AG-02 联动,风格闸门支持 skip,StyleApplyAll/Deliver 按无风格走(管线已天然支持 `chosen_recipe=None` 的空跑)。空描述软化为 skip、StyleApplyAll 见 chosen=None 自动推进到交付闸门。
2. **Telegram /命令快路径**:/status、/cancel、/help 注册为 bot commands,确定性零 LLM 零延迟,与按钮互补(按钮只在闸门消息上,命令随时可用);本地模型分类 10s+ 首响时价值明显。
3. **进度消息原地编辑**:收图播报与 Evaluate 轮询播报改用 editMessageText 更新同一条消息,长批次不再每 60s 刷一条屏。
4. **风格候选一览** ✅ 基础版已随上述增量落地(2026-07-18):Style 问描述闸门的 query 分类返回 9 个 preset 名 + 一句话描述(新增 `style_matcher.describe_presets()`,单一数据来源 `_PRESET_DESCRIPTIONS`);进阶版发多风格对比预览(算力代价大)仍后置。
5. **多会话/多 chat_id**:Eng Design 已明确留给部署周,不预留半成品抽象,此处仅记录。

难度 各 S-M | 优先级 P3(1、2 可随 P1/P2 顺手立项)

---

**AG-17 get_updates 失败无退避,故障期 0.1s 热重试**
类别: 健壮性 | 来源视角: 工程

`transport/telegram.py:59-62` 对 get_updates 异常统一 `sleep(0.1)` 后重试,断网/Telegram 故障期约 10 次/秒空转并刷屏打印。改指数退避(0.1s 起、封顶 30s,成功即复位)即可。

难度 S | 复杂度 低 | 优先级 P3

---

**AG-18 视频等不认识的消息形状只在服务端打印,用户侧无反馈**
类别: 交互打磨 | 来源视角: 用户

`_handle_update` 对非 photo/document/text 消息(视频、语音、贴纸)只打服务端诊断日志,用户发视频后完全静默。回一句"目前只支持照片哦"即可,与下载失败的既有回执风格一致。

难度 S | 复杂度 低 | 优先级 P3

---

**AG-19 管道形状在 5+ 处硬编码,"加一个能力 = 加一个 Stage"名不符实**
类别: 观察记录 | 来源视角: 架构

七段固定管线分散在:plan_composer 的 stages 列表、validate 的 `_EXPECTED_STAGE_NAMES`、run_telegram 的 stages dict(含 Deliver `inputs=["StyleApplyAll"]` 覆盖)、consumer 的参数注入(Ingest/Deliver)、worker `_prepare_gate_payload` 的按 stage 分支、consumer 的按闸门渲染分支。加一个 Stage 实际要动 5-6 个文件。当前规模(7 个 stage、一条固定管线)下抽"管线模板单一来源"尚不划算,且与"不做超范围抽象"契约一致;记录在案,等下一个真实新 Stage 需求出现时再决定是否抽(届时优先抽"模板 + validate 共用一份声明",闸门 payload/渲染的分发可以不动)。

优先级 P3(暂不行动)

---

**AG-20 小额清理打包**
类别: 纯清理 | 来源视角: 工程

1. `orchestrator/driver.py:23` 的 `transport` 参数:M4 占位,如今 Deliver 直接持有 transport,该参数无人传递,删。
2. `SessionWorker.__init__` 七个分类函数逐个具名注入(worker.py:59-79),改传一个 `dict[kind, fn]` 注册表,`_execute_classify` 的 if/elif 链同步变查表,构造与调用两端各省十几行。
3. consumer 内重复文案就地收敛(如"还没告诉我想怎么处理呢..."出现两次、"这个选项已经过期了"两次),抽模块级常量即可,不需要模板层。
4. `_cancel_confirm_pending` 挂起期间收到照片不清确认态(用户发新照片显然不想取消了),`_handle_photo` 顺手置 False,与文本 other 分支的"安全撤销"语义对齐。

难度 S | 复杂度 低 | 优先级 P3

---

**AG-21 常驻进程只有 print,无带时间戳的落盘日志**
类别: 工程规范 | 来源视角: 架构

consumer/worker/transport 的可观测性全靠 stdout print(真机反馈刻意加的,方向正确),但常驻 daemon 场景下无时间戳、无落盘,事后排查全凭终端 scrollback。换 `logging`(带时间戳,console + `~/.pzt-agent/agent.log` RotatingFileHandler)是纯机械替换,print 语句一比一映射。

难度 S | 复杂度 低 | 优先级 P3

---

## 修复顺序建议

1. **第一波(半天到一天)**:AG-03(协议加字段,防状态污染)→ AG-05(StageStarted 时机)→ AG-06(交付失败误报成功)。三条都是 S 难度、真机每批可见或危害大。**✅ 已完成(2026-07-18): AG-03 `c5ada33`、AG-05 `d5a8493`、AG-06 见本轮 commit。**
2. **第二波(一到两天)**:AG-01 + AG-02 + AG-16.1 作为一个"Style 闸门健壮化"小增量一起做(同一片代码,分开做会互相返工);顺手 AG-04(闸门重复提问)。**✅ 已全部完成(2026-07-18): Style 闸门健壮化(AG-01+AG-02+AG-16.1,顺带 AG-16.4 基础版) commit `fd8f1d1`; AG-04 见本轮 commit。**
3. **第三波**:P2 里按真机痛感排(建议 AG-09 caption、AG-10 retryable、AG-08 先行,AG-11/AG-12/AG-14 属于"跑得越久越重要"的后台健壮性,部署周(W2026-07-15 目标四)开始前做完最合适)。
4. P3 不排期,随手或等触发。
