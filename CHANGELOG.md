# Changelog

> 本文件由 git-cliff 从 commit 历史自动生成,**请勿手改**(下次发布会覆盖)。
> 发布与生成流程见 `scripts/release.sh`。

## v2026.7.24 - 2026-07-24


### 📝 文档
- Docs: 更新 README/agent README/主页，去掉过时的"打分"描述
- Docs: W2026-07-21 周目标全部收口，归档进 history
- Docs: Task_Pool 新增锦标赛双输判定条目(pairwise 允许双硬伤都不进候选)
- Docs: W2026-07-21 目标三 Eng Design(dedup/选片流程可选化，Commit 6-9 任务分解)
- Docs: W2026-07-21 PRD 新增目标三(dedup/选片流程可选化)，收窄手动选片为延后项
- Docs: 目标一任务分解按可构建顺序重排(下游解耦先于删分数函数)
- Docs: W2026-07-21 周目标 PRD + 目标一 Eng Design(eval 解耦 + core pairwise 地基)

### 🔩 其它
- agent: 选片确认挪到滤镜之前, 简化最终交付(真机反馈)
- agent: 修复"去重+筛N张"数量识别失败, 调整确认文案措辞(真机反馈)
- agent: 去重确认精简 + 追问后打字给数量要二次确认(目标三真机反馈)
- agent: 修复 dedup_followup classify 崩溃(目标三真机验证发现)
- agent: 修复"去重"意图在收图阶段识别不出来的问题(目标三真机验证发现)
- agent: AI 可发现性提醒文案 + 快捷按钮(目标三 Commit 9), 收官
- agent: Dedup 后追问闸门接上会话层(目标三 Commit 8), 修复 Commit 6 遗留的两个消费侧回归
- agent: Curate 新增 passthrough 模式(目标三 Commit 7): count=None 时不聚类, 原样交付去重幸存者
- agent: Plan 形状按意图可选(目标三 Commit 6): count 给了跳过 Dedup、只说去重先 Dedup 再问
- feat(agent): 接入全局 AI 开关(ai_enabled/provider)
- refactor(agent): 删除 Evaluate stage 及全部引用
- feat(cli): pzt dedup/curate 接上 --ai/--provider
- feat(core): dedup/curate 收口成 cluster_and_choose 的调用方
- feat(core): 新增 core::tournament::cluster_and_choose 锦标赛原语
- docs: W2026-07-21 目标二 Eng Design(dedup/curate 锦标赛 + 全局 AI 开关)
- refactor(cli): 去掉 PZT_LANG/PZT_AI_PROVIDER 环境变量覆盖
- fix(core): 收紧 eval unusable 判据的 prompt 措辞
- refactor(core): eval 落库改存单列 result_json, 不再拆 assessment/unusable 两列
- fix(cli): recipe 菜单补回 0:[Origin] 展示项
- fix(cli): 控制台折行第二行留前导空格 + recipe 菜单编号选项铺满两行
- fix(core): eval assessment 始终用界面语言, 不跟随 guidance
- fix(cli): AI 点评信息栏改标题为"AI 点评", 可用不显示状态, 不可用加粗提示
- feat: core pairwise 视觉比较能力 + pzt compare headless
- feat: eval 重构为"文字 assessment + unusable flag", 移除跨图分数
- feat: dedup/curate 脱离 AI 分数(留最新+排废片 / 纯标签候选+时间多样性)

### 🚀 部署与分发
- Deploy: 回填 pzt/pzt-agent formula 到 v2026.7.21 (sha256)
## v2026.7.21 - 2026-07-21


### 📝 文档
- Docs: 新增 docs/RELEASE.md(一次性 GitHub 设置 + 发版流程); bottle/安装统计进 Task_Pool
- Docs: W2026-07-15 周目标全部收口, PRD 归档进 history, 标记部署完成

### 🔩 其它
- Release v2026.7.21
- pzt: ai_provider 默认改 Local(本地 Ollama), 不再默认 Gemini

### 🚀 部署与分发
- Deploy Phase D: 一键 dispatch release 自动化(CalVer + 构建闸门)
- Deploy: 主页文案标点全角化 + ai_provider 默认标 local
- Deploy Phase C: 主页加配置一节(pzt CLI config.json + agent 环境变量)
- Deploy Phase C: 静态主页 + GitHub Pages 部署 workflow
- Deploy: 回填 pzt/pzt-agent formula 到 v2026.7.20 (sha256)
## v2026.7.20 - 2026-07-20


### 🔩 其它
- Release v2026.7.20

### 🚀 部署与分发
- Deploy Phase B: pzt-agent formula 修正(ptb 用 wheel 绕过 sdist 构建失败 + audit 风格)
- Deploy: gitignore agent egg-info(pip install 产物)
- Deploy Phase B(2/2): pzt-agent formula + README + 启动脚本 + release.sh 扩展
- Deploy Phase B(1/2): agent 可 pip 安装 + default_pzt_bin PATH 兜底
- Deploy: README 安装步骤补 brew trust(Homebrew 6 第三方 tap 信任门)
- Deploy: pzt formula depends_on 按 brew audit 字母序排列
- Deploy: 回填 pzt formula 到 v0.1.0 (sha256)

### 🤖 Agent
- Agent: 修交付上传超时崩溃(放大上传超时 + 有界重试)
## v0.1.0 - 2026-07-20


### 📝 文档
- Docs: 几何变换(裁切/水平矫正)从本周目标二顺延, 收进 Task_Pool
- Docs: W2026-07-15 四份 Eng Design 归档进 history, PRD 标记目标完成状态
- Docs: Fix-it 评审归档进 history,未完成中长期项提炼为 Task_Pool
- Docs: Agent Fix-it 收口, 评审清单归档到 history
- Docs: SPEC agent 模块清单加 session 多线程运行时, router 标注仅剩 collecting 纯函数
- Docs: 目标五 AgentRuntime Eng Design(双线程运行时/所有权协议/取消协议), 同步修正 PRD 可杀白名单表述
- Docs: W2026-07-15 PRD 追加目标五(agent 运行时 consumer/worker 双线程重构)
- Docs: 只把 SPEC.md 定为每个 session 必读，其余文档按 session 开发意图按需加载

### 🔧 Fix-it
- Fix-it F-44: 拍板维持推迟,写清批量打标签的触发前提
- Fix-it F-28: 拍板有序标签顺序=打标签顺序,调序降为已知边界
- Fix-it F-41: 拍板保持 pzt list 轻量,per-tag 计数记为已知偏离
- Fix-it F-37/F-38: Wave 2 编译时长/每帧开连接实测,均维持观察
- Fix-it F-34/F-36: raw alpha 补不透明 + 工程规范杂项(短写/sleep_for/p95/dhash optional)
- Fix-it F-35/F-42: is_wide_codepoint 补 emoji + read_text_line 超宽两行换行
- Fix-it F-39/F-40: dedup 分组顺序确定化 + load_metas IN 按 500 分块
- Fix-it F-14/F-15: prefetch 消持锁拷贝整图与每键全项目路径表
- Fix-it F-24: 会话续点(重开项目回到上次浏览位置)
- Fix-it F-20: 终端 resize 残影修复(主动 poll 自动恢复)
- Fix-it F-22: 控制台/文本纯函数抽入 cli/text 并补测
- Fix-it F-16: 抽 core/media, 消反向依赖与三份 raw 判断

### 🔩 其它
- Split the 2026-07 Fix-it Night doc: archive the completed report to history/, trim the live doc to remaining items
- Update AgentStyle Eng Design to the shipped text-description two-stage flow
- Reorganize docs: archive completed milestone docs to history/, add global SPEC.md
- Wire the new Style/StyleApplyAll split into run_telegram.py only, keep run_watchfolder.py/run_intent.py on the old Curate->Deliver shape
- Wire the two-stage gated Style flow into SessionRouter
- Insert Style/StyleApplyAll as two required-gate stages between Curate and Deliver
- Point DeliverStage at StyleApplyAll's output instead of Style's
- Add StyleApplyAllStage to apply the confirmed style to the rest of the batch
- Rewrite StyleStage to match one preset from a text description instead of per-photo vision
- Add Driver.rerun_stage to re-run a gated stage without re-asking its gate
- Add text-only style description matcher
- Add a periodic 'N photos received' progress ping while collecting
- Send a progress message at each stage transition while RUNNING
- Add Driver.peek_next_stage for progress-reporting hooks
- Fix debug log truncation to not cut multi-byte UTF-8 characters, crashing the agent's UTF-8 stderr decode
- Document the provider-default-to-local change and the router test fragility it exposed
- Default both the meta-provider and vision-provider to local (Ollama) to avoid cloud quota
- Add local (Ollama) support to the Python meta-provider text LLM client
- Correct the Eng Design doc: Style has no gate of its own, Deliver's existing gate reviews the picks
- Fold the applied Style recipe into Deliver's dedup marker so style-only adjustments trigger redelivery
- Wire Style into compose_plan and all three agent entry points, with Deliver's existing gate reviewing the style picks
- Add StyleStage: shells out to pzt recipe suggest/apply per selected photo
- Add pzt recipe suggest/apply headless commands
- Add core::ai::style for LLM-based recipe suggestion with post-parse hallucination validation
- Add Eng Design doc for the agent Style stage (LLM auto-picks a recipe)
- Add a short (-100~100%) hint to each version-param prompt in the r-menu create flow
- Wire contrast/saturation/blacks/whites into the r-menu create flow and recipe list display
- Add contrast/saturation/blacks/whites to VersionParams and thread them through render
- Extend apply_adjustments with blacks/whites/contrast/saturation via a new AdjustParams struct
- Document the contrast/saturation/blacks/whites version params extension
- Filter Origin out of presets_for_menu so the 9 City+Year presets fill keys 1-9
- Replace the Warm placeholder preset with the 9 City+Year built-in presets
- Add recipes.grain_amount and thread it through resolve_recipe/render into apply_grain
- Add detail::GradeParams/make_graded_lut to bake preset LUTs from numeric grading knobs
- Add core::color::apply_grain, a deterministic position-hashed grain pass
- Add Eng Design doc for the City+Year recipe preset system
- Record real Task 5 benchmark results and the schema-constrained-decoding decision in the Eng Design doc
- Change the default Provider::Local model from moondream to gemma4:e2b
- Add JSON-Schema-constrained decoding + temperature=0 for Provider::Local evaluation requests
- Wire --provider local into cmd_eval and the interactive pzt open path
- Add this week's dev-goals PRD and the local-model Eng Design doc
- Thread LocalModelConfig through EvaluationFn/EvaluationWorker/request_evaluation
- Wire Provider::Local into request_json: build_local_request/parse_local_response, three-way branch
- Add Provider::Local enum value, LocalModelConfig, and Settings.ollama_base_url/ollama_model
- Assert optional-stage failures stay visible in run.outputs through to AwaitingReview
- Assert pzt eval --auto-reject does not mutate global Settings
- Add a determinism regression test for core::curate::curate
- Fix Deliver idempotency: mark each file as sent immediately after sending, not after the whole batch
- Wire classify_collecting_message into run_telegram.py's build_router
- Wire classify_collecting_message into Collecting, with fallback to prior behavior on any classification failure
- PLANNED: stop auto-running after a plan correction, add natural-language approve/reject
- Add _status_snapshot_text and wire query recognition into AwaitingGate and PLANNED
- Add query recognition to classify_gate_reply/refine_plan_confirmation, plus new classify_collecting_message
- Report failed downloads to the chat, and don't let one oversized preview photo kill the whole preview
- Wire refine_plan_confirmation into run_telegram.py's build_router
- Wire refine_plan_confirmation_fn into PLANNED: merge specific corrections, ask for clarification on vague ones
- Add PLANNED confirmation step: propose a Plan and wait for approve/reject before running
- Add refine_plan_confirmation (merge-or-clarify LLM call for the PLANNED confirmation step)
- Add SessionRouter.check_idle_timers() and wire it into run_telegram.py's main loop
- SessionRouter: touch last_activity_at per message, queue new photos at the gate instead of merging
- Add router/collecting.py pending-photo queue (queue_incoming_photo/drain_queue_into)
- Add RunState.last_activity_at/reminder_sent for idle-timer tracking
- Recognize casual approval/rejection at the gate via LLM, not just exact keywords
- Raise Telegram HTTP timeouts and stop the main loop from dying on one bad message
- Fix TelegramTransport dropping images sent as files, and dying silently on per-update errors
- Add run_telegram.py (Telegram polling entry point wiring TelegramTransport + SessionRouter)
- Add SessionRouter gate dispatch: approve/reject/adjust at the gate, RUNNING/AwaitingReview self-heal
- Add SessionRouter core: Collecting handling, intent-to-Plan drive, gate preview
- Add router/collecting.py (Collecting-run RunState + incoming-photo staging)
- Add transport/telegram.py (TelegramTransport: sync/async bridge over TelegramBotClient)
- Add transport/telegram_client.py (Telegram Bot API wrapper + env config)
- Add offline eval script for compose_plan/parse_adjustment prompts
- Fix DeliverStage marker to key off delivered selection, add adjustment integration test
- Add run_intent.py (intent-driven watch-folder runner with adjustment loop)
- Add CurateStage exclude param (swap-out support via over-fetch and retag)
- Add compose/adjustment_parser.py (parse_adjustment: chat message to PlanDelta)
- Add compose/plan_composer.py (compose_plan: intent text to Plan via LLM)
- Add compose/validate.py (deterministic Plan guardrail for LLM output)
- Add compose/llm_client.py (text-only Python mirror of core/ai/ai.cpp)
- Fix DeliverStage: export to a private staging dir instead of the transport's out_dir
- Add PZT_FAKE_EVAL escape hatch to pzt eval (canned passing scores, no real AI call)
- Switch default Gemini model to gemini-3.1-flash-lite (higher RPM quota)
- Add run_watchfolder.py (fixed-Plan runner tying Driver + real Stages + WatchFolderTransport together)
- Add real Stage implementations (Ingest/Evaluate/Dedup/Curate/Deliver) wired through pzt_client
- Add Transport protocol and WatchFolderTransport (one-shot batch, dev/test transport)
- Add pzt_client.py (subprocess bridge to headless pzt commands)
- Add deterministic orchestrator Driver (sequencing, gates, failure handling, checkpoint resume, adjustment)
- Add RunStore (JSON-file-per-run persistence for agent orchestrator)
- Add agent/ scaffold and orchestrator Stage/Plan/Run data types
- Stop curate from backfilling near-duplicates when clusters < N
- Add pzt tag clear headless command for agent re-curate workflows
- Add pzt curate --json headless command (greedy diversity selection + configurable apply-tag)
- Add core::curate clustering + greedy diversity selection algorithm
- Add curate_time_window_seconds/curate_hash_threshold settings (looser than dedup defaults)
- Clarify curate clustering threshold must be looser than dedup's
- Add --json output to pzt new for headless ingest
- Add pzt eval --json headless command (sync batch, explicit auto-reject)
- Thread eval auto-reject as an explicit param (isolate from global Settings)
- Add pzt export-images --json headless command
- Add pzt dedup --json headless command + shared scope resolver
- Add pzt tag apply verb (path-addressed, --on-cap policy)
- Add pzt images --json headless command + smoke harness
- Add find_image_by_path facade for headless path addressing
- Add M4 增量一 Eng Design (engineering realization + verifiable sub-increments)
- Switch M4 v1 transport to Telegram, move WhatsApp/other IM to TODO
- Add M4 增量一 PRD: v1 cull-deliver closed loop + WhatsApp
- Add M4 agent-workflow design spec, reframe brainstorm as broad reference
- Add M4 brainstorm doc (agent layer, use cases, local-model strategy)
- Record the post-manual-verification round of fixes in Fix_It_Night_Review.md
- Confirm quit with pending eval tasks, reorder tag menu, compact filter label, fix case-insensitive Reject/Duplicate alias
- Record the post-batch-3 E2E testing round in Fix_It_Night_Review.md
- Rename the filter menu key from g to f (point 3)
- Redesign the e export key, retire the g+e tag-picker flow (point 2)
- Make tag name matching and uniqueness case-insensitive for ASCII
- Resolve #Reject/#Duplicate console scope names regardless of UI language
- Add /help [command] console command
- Add Settings.auto_ai_reject to auto-tag failing evaluations as reject
- Add console /filter two-tier filtering on top of the g-tag view (F-09)
- Read dedup thresholds from Settings, count no-capture-time skips, log hamming distances (F-08)
- Default-exclude reject/duplicate images from ai_eval, dedup, and export (F-26)
- Add entry hint to dedup result message when duplicates are tagged (F-11)
- Add core::settings: global config.json for provider/dedup/exclusion/UI defaults (F-12)
- Batch-query evaluated status, remove dead DedupSummary field (F-07)
- Bind the "重复" duplicate tag to slot 9 in space/space-/g menus (F-01)
- Record finalized decisions for F-01/F-08/F-09/F-12/F-13/F-26, add schedule
- Add execution log for the fast-fix package run
- Refresh AGENTS.md's stale M3 status (F-23)
- Remove three small pieces of dead weight (F-31, F-32, F-33)
- Harden curl usage: NOSIGNAL and a real connect timeout (F-21)
- Flush buffered stdin after long blocking operations (F-25)
- Harden write-path error handling and Result<T,E> (F-17, F-18, F-19)
- Make the AI provider configurable via PZT_AI_PROVIDER (F-10)
- Surface AI evaluation failures to the user (F-03)
- Downscale images before AI upload (F-02)
- Harden directory scanning and reject unknown pzt new flags (F-06)
- Set sqlite3_busy_timeout on every connection (F-04)
- Add exception boundary in main() (F-05)
- Add 当前状态 tracking column to fix-it night matrix
- Add fix-it night review report covering product/user/architecture/engineering perspectives
- Break dedup keep_id score ties by most recent capture, not image_id
- Support left/right cursor movement in text input, fix arrow-key exit
- Support #"quoted tag name" scopes in /ai_eval and /dedup
- Fix system tags other than Reject always displaying as "Reject"
- Open a project right after pzt new, pack tags onto one info-panel line
- Implement /ai_eval and /tasks, require / for every console command
- Wire /dedup into the pzt open AI console
- Add ensure_duplicate_tag and the find_and_tag_duplicates facade
- Add core/dedup: time-clustering + dHash near-duplicate detection
- Design AI console commands and near-duplicate detection for M3
- Rework AI scoring into structured culling assessment
- Narrow M3 increment-1 PRD scope from aesthetic scoring to culling assistance
- Wire the AI console into pzt open, refined against real usage
- Add core::ai::ScoreWorker - async, deduplicated scoring with DB writeback
- Add ai_score columns and extend ImageInfo/get_image
- Add core::ai::score - aesthetic scoring, request_json's first consumer
- Add core::ai::request_json generic AI request layer
- Add M3 increment-1 PRD and Eng Design: aesthetic scoring, manual trigger
- Split the bottom banner into two rows to stop long option lists overflowing
- Keep space/r submenus open after creating a tag or recipe version
- Rework pzt open's top-level menu: right-side panel, ordering, and feedback
- Make RAW support an opt-in, hidden feature gated by --support-raw
- Update stale docs: AGENTS.md status marker, M2 Eng Design wrap-up record
- Sort by capture time descending - newest first
- Show capture time in the info bar, minute precision
- M2 wrap-up issue 3: sort by capture time instead of filename
- M2 wrap-up issue 2: top-level e key exports the current photo
- Fix RAW progress indicator firing after decode instead of before
- M2 increment 6: final acceptance run against PRD checklist
- Document Leica Q3 vignetting / missing lens correction as a future item
- Remove pzt export --link (symlink mode)
- M2 increment 5: export routing for kind x recipe (4 states)
- M2 increment 4: wire the preview path to actually use the RAW cache
- M2 increment 3 (rework): drop RAW+JPEG pairing, generate half_size preview cache
- Revise M2 design: drop RAW+JPEG pairing, add half_size preview cache
- M2 increment 4: rescan pairing-upgrade logic
- M2 increment 3: schema migration + RAW/JPEG pairing scan
- Document in-camera style loss as a known M2 limitation
- M2 increment 2: real core/raw implementation
- M2 increment 1: wire LibRaw into the CMake build
- Add M2 PRD, eng design doc, and Phase 0 LibRaw performance spike
- Move export skip-reason text out of core into cli/i18n
- rename 'Clear filter' to 'Clear Filter'
- Unify menu item format to key:[Label] across all interactive menus
- Add cli i18n (zh/en) support, done in a separate session; fix gaps found on review
- Split cli/main.cpp (1867 lines) into cohesive cli modules
- Small refactor pass before M2: dedupe menu boilerplate, center image
- Complete M1 increment 8: integration and acceptance
- Implement M1 increment 7: export baking
- Fix real-terminal issues found in increment 6 r-menu review
- Implement M1 increment 6: full r-key interactive menu
- Implement M1 increment 5: preview integration, r v toggle, Origin preset
- Implement M1 increment 4: color rendering pipeline + JPEG encoding
- Implement M1 increment 3: image-recipe association + info bar display
- Implement M1 increment 2: version CRUD and soft delete
- Implement M1 increment 1: recipes table, images.recipe_id migration, seed presets
- Add M1 PRD, eng design doc, and Phase 0 color pipeline spike
- Implement increment 6.6: g+e export shortcut, fix export path bugs
- Document g+e export shortcut design (increment 6.6, not yet implemented)
- Ignore build_release/, a separate RelWithDebInfo build dir for subjective testing
- Check off M0 acceptance criteria, verified manually against the real build
- Implement increment 6.4.7: retire debug commands, latency summary, 500+ acceptance test
- Implement increment 6.4.6: g + digit filter view, g+g clear filter
- Implement increment 6.4.5: 废片 system tag fixed to slot 0, x key
- Implement increment 6.4.4.5: space -/c/d tag management, plus real-terminal fixes
- Finalize tag-management keybinding design: fix 废片 to slot 0, space -, keep g
- Document the full space-menu tag management design (add/remove/create/delete)
- Implement increment 6.4.4: space quick-tag menu with cap-exceeded replace
- Fix banner text truncation by properly computing UTF-8 display width
- Document tmux passthrough's inactive-pane limitation as accepted for M0
- Revert "Clear the Kitty image when tmux switches away from pzt open's window"
- Clear the Kitty image when tmux switches away from pzt open's window
- Fix PrefetchCache worker not noticing stop until its queue drains
- Don't re-render the whole screen on an unsupported keypress
- Make rescan prune stale image records by default
- Add ASCII box borders to pzt open and cap width at 70% of terminal
- Fix three real-terminal bugs found testing 6.4.2: image residue,
- Implement increment 6.4.2: three-panel layout, fix overlapping renders
- Implement increment 6.4.1: cbreak mode + real h/l browse loop
- Implement increment 6.3: core/browse prefetch ring buffer
- Implement increment 6.2: cli/kitty Kitty graphics renderer
- Implement increment 6.1: core/decode JPEG decoding module
- Add M0 eng design doc and core/cli implementation through export (increments 1-5)
- Initial commit: project docs (AGENTS.md, Roadmap, M0 PRD)

### 🚀 部署与分发
- Deploy Phase A: CLI 分发脚手架(README + formula + CHANGELOG + release 脚本)
- Deploy Phase 0: 版本真相源 + install 规则(pzt --version)

### 🤖 Agent
- Agent: AG-21 print -> logging(带时间戳、落盘 agent.log)
- Agent: AG-20 小额清理打包(四项)
- Agent: AG-18 不认识的消息形状给用户回执, 不再静默
- Agent: AG-17 get_updates 失败改指数退避, 不再 10 次/秒空转
- Agent: AG-16.3 进度消息原地编辑, 长批次不再刷屏
- Agent: AG-16.2 Telegram /命令快路径(/status /cancel /help)
- Agent: AG-15 Deliver 预览逐张带"第 N 张"编号, 失败占位保序
- Agent: AG-13 语言 provider/本地模型名可经环境变量切换, 无需改代码
- Agent: AG-07 inline 按钮服从文本 FIFO 串行, 不再用旧参数抢跑
- Agent: AG-14 状态目录清理/保留策略(含 headless pzt delete, N=7 天)
- Agent: AG-12 bootstrap 自愈取代 assert, 取消过的 run 不复活
- Agent: AG-11 consumer 单条消息/事件出错不再连累同批
- Agent: AG-10 消费 ClassifyFailed.retryable, 区分"AI 连不上"与"没看懂"
- Agent: AG-09 照片 caption 里的意图接入文本管线, 不再丢弃
- Agent: AG-08 草稿态零照片不再组空方案, 补充意图不覆盖草稿
- Agent: AG-04 Deliver 闸门调整选片后不再重问风格
- Agent: Style 闸门健壮化(AG-01+AG-02+AG-16.1, 顺带 AG-16.4 基础版)
- Agent: AG-06 交付导出失败改判 run FAILED(不再误报"处理完啦")
- Agent: AG-05 修带闸门 stage 的 StageStarted 误发时机
- Agent: AG-03 修 JobCrashed 不区分 lane(classify 崩溃污染 drive 会话)
- Agent: run_telegram2 转正为 run_telegram, 删除旧单线程 router.SessionRouter 及其测试(真机验证通过)
- Agent: 修四个真机问题(换滤镜预览不刷新/改参数被当query/闸门idle计时器在等AI时误触/意图先于照片被丢) + 加草稿方案(draft)与start动作
- Agent: 方案确认文案点明也会去重(自动剔除不合格和重复的照片); False 分支同步为'只去重复、保留不合格的照片'(去重总会做)
- Agent: 彻底去文本精确匹配, 全交 LLM 分类(新增 style_gate/running/cancel_confirm 三分类器) + worker 拆双 lane(classify/drive 并发, 处理中也能跑取消/进度分类); 按钮回调保持确定性
- Agent: 去掉所有取消提示语/取消意图交LLM(collecting cancel+闸门reject走二次确认)/非选图消息给help不硬编默认方案/交付完成补批次收尾语
- Agent: 新建 collecting run 时立即回一句确认(初始那波图后台下载时不再静默, 每批只发一次)
- Agent: 取消安全化(确认按钮去掉取消/取消只能打字触发+二次确认[确认取消][不取消]/drive 中也先确认再掐)
- Agent: 所有 yes/no 确认点改 inline 按钮(transport callback_query + consumer 按钮映射/run_id 防误触/无能力降级), 修风格闸门精确匹配踩坑
- Agent: 真机反馈调整(加回控制台调试日志/JobCrashed 通知用户/确认文案去 provider/区分 AI 连不上/apply_tag 依受众取名)
- Agent: run_telegram2.py 双线程入口 wiring + 真线程端到端冒烟(consumer/worker/两队列在真并发下走通全流程)
- Agent: session/consumer.py(消息线程: 文本串行/关键词快路径/事件应用/闸门文案渲染/timers/启动续跑)
- Agent: session/worker.py(串行 job 执行器: drive 推进/闸门媒体准备/可杀布防/取消收尾), 协议删掉无发射方的 StageProgress
- Agent: PztClient 可取消调用(实例布防 cancel_event, terminate/kill 两段收尾, PztCancelledError 穿透 stage)
- Agent: session/protocol.py + view.py(2.0 运行时的 Job/Event 协议与会话视图)
