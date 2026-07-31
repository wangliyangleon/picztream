# 历史文档归档

这里存放已经完成或被取代的里程碑级文档，不需要每个 session 都加载。长期 ground truth 见 `docs/SPEC.md`，本周细节见 `docs/W{日期}_*`。需要追溯某个能力"当初是怎么设计的、为什么这么定"时，再按下表查阅对应文档。

## 里程碑 PRD / Eng Design

- `M0_PRD.md` / `M0_Eng_Design.md` - MVP，选图管理核心（全键盘、项目化、带上限标签分组，纯 JPEG，零 AI/网络依赖）
- `M1_PRD.md` / `M1_Eng_Design.md` - 风格化，`recipe.json` 色彩配方系统与同步预览
- `M2_PRD.md` / `M2_Eng_Design.md` - RAW 支持，LibRaw 接入，双层流水线打通（RAW 当前基线与风险见 `docs/RAW_Support.md`）
- `M3_PRD.md` / `M3_Eng_Design.md` - AI 辅助之选片辅助评分（主动触发）
- `M3_Dedup_PRD.md` / `M3_Dedup_Eng_Design.md` - AI 辅助之近似重复检测去重
- `M4_PRD.md` / `M4_Eng_Design.md` - Agent 半自动增量一，Telegram 选片-交付闭环
- `M4_Agent_Workflow_Design.md` - agent 编排层架构设计（Stage/Plan/Driver/闸门/调整模型的原始设计）
- `M4_Brainstorm.md` - M4 前期头脑风暴（agent 层、用例、本地模型策略的广义参考）

## 周开发目标（W2026-07-15，已收口归档）

本周五个目标已全部收口（2026-07-20），PRD 与各目标的 Eng Design 均已归档到这里：

- `W2026-07-15_PRD.md` - 本周开发目标 PRD（五个目标：本地模型 / recipe 扩展 / agent Style / 部署分发 / agent 运行时重构）。每个目标小节顶部有状态行；目标二的几何变换顺延，见 `docs/Task_Pool.md`

以下四份 Eng Design 的落地形态与文档一致，各自顶部有归档说明（目标四部署无独立 Eng Design，工程细节落在 formula/脚本/workflow 与 README）：

- `W2026-07-15_LocalModel_Eng_Design.md` - 目标一，本地模型 `Provider::Local`/Ollama（默认 `gemma4:e2b` + JSON-Schema 约束解码，第十节含真机基准）
- `W2026-07-15_RecipeExpansion_Eng_Design.md` - 目标二，Recipe 预设扩展（9 个 City+Year 预设 + `VersionParams` 四新旋钮 + 数值规范化；**几何变换未做、顺延**，见文档归档说明）
- `W2026-07-15_AgentStyle_Eng_Design.md` - 目标三，agent 选风格（最终形态=文字描述+纯文本匹配+整批统一两段式闸门，第一节记录相对 PRD 的翻转；第七节 router 接入已被目标五 consumer 取代）
- `W2026-07-15_AgentRuntime_Eng_Design.md` - 目标五（2026-07-17 追加），agent 运行时 consumer/worker 双线程重构（含多轮真机反馈的刻意偏离，第八节）

## 周开发目标（W2026-07-21，已收口归档）

本周三个目标已全部收口（2026-07-24），PRD 与两份 Eng Design 均已归档到这里：

- `W2026-07-21_PRD.md` - 本周开发目标 PRD（三个目标：eval 解耦地基 / AI 锦标赛接线 / dedup-选片流程可选化）。手动选片模式已明确移出范围，延后到未来单独立项

以下两份 Eng Design 的落地形态与文档一致，各自顶部有归档说明：

- `W2026-07-21_Eval_Eng_Design.md` - 目标一，eval 解耦（`overall_score`/`passes_gate` 移除，改产文字描述 + `unusable` flag）+ core pairwise 比较地基
- `W2026-07-21_Tournament_Eng_Design.md` - 目标二（dedup 两类 + curate 两模式 + 全局 AI 开关，Commit 1-5）+ 目标三补充设计（dedup/选片流程可选化，Commit 6-9，文内单独一节）

实现完成后的真机反馈还触发了一次超出本周 PRD 范围的架构调整（选片确认闸门挪到滤镜之前、Deliver 不再挂闸门），归档说明里有指路，具体没有独立文档，改动落在 `agent/session/consumer.py`/`worker.py` 的对应 commit 里。

## 单点增量（不属于任何一周的目标）

- `Dedup_AI_Console_PRD.md` - 控制台 `/dedup <范围> --ai`（2026-07-29 收口）。W2026-07-21 的 AI 锦标赛此前只有 agent 层通过 headless `pzt dedup --ai` 接得上，交互控制台没有入口；这一刀补上开关、开跑前的开销闸门（报精确的组数/比较次数，取消则零写入）与分组/比较两段进度反馈，并删掉随那轮改造一起失效的"未评估"确认。文档篇幅小、没有独立 Eng Design，实现记录与偏离说明直接写在 PRD 里
- `Env_Preflight_PRD.md` / `Env_Preflight_Eng_Design.md` - 环境前提不满足时告知原因（2026-07-30 收口，提案 T-10）。三个必须满足的运行前提（Kitty 协议终端、Telegram 凭证、Ollama 可达）不满足时都会失败，但没有一处告知原因；最恶劣的是非 Kitty 终端下渲染是"静默成功"的（`write()` 返回值等于长度、`RenderError` 不涵盖这种情况），产品核心卖点表现为一个只是不显示照片的完整界面。这一刀补上终端白名单探测 + 提示、Telegram 凭证的人话错误、Ollama/API key 的启动期预检（仅告警不拦启动），顺带救活两条被 `DebugLogRedirect` 吞掉、永远没人看得见的死文案。**真机验收推翻了 PRD 的"提示不阻塞"与 Eng Design 的"提示画进 banner"**，返工记录与方法论教训写在两份文档顶部的归档说明里
- `Headless_Observability_PRD.md` / `Headless_Observability_Eng_Design.md` - headless 长运行的进度、降级信号与取消（2026-07-31 收口，提案 T-8）。`--ai` 把一次 headless 调用推到分钟级之后，坐在终端前的人那一侧已经连收四刀（T-9a/T-9b/T-14/T-10），Telegram 那一侧一次都没收过：确认方案后一句"正在筛选..."然后分钟级沉默，期间不知道跑到哪、不知道能不能停、结束后分不出结果是 AI 真跑通的还是超时退化的。这一刀补上三条：**进度走 stderr 的带外通道**（stdout 的原子性一字节不变，SPEC §3.2 的契约表述相应改写）、`ai_fallback_count` 不再在 Curate 路径上蒸发并进入用户话术、`Style`/`StyleApplyAll` 补进可取消集合（带部分完成回执）。**真机验收推翻了 PRD 决策三"把 phase 压掉"**，返工两轮，教训写在 PRD 顶部的归档说明里。Eng Design 只覆盖三个有真设计风险的问题（stderr schema、curate 签名、流式读不踩管道死锁），其余按 PRD 直接实现
- `Dedup_Cancel_PRD.md` - `/dedup` 跑到一半按 Ctrl-C 取消（2026-07-29 收口）。上一条把进度补到每一次比较之后，紧跟着的诉求就是"看着它走、然后不等了"——而当时唯一能按的 Ctrl-C 会杀掉整个 `pzt open`。这一刀让取消只作用于这一次去重：下一次比较边界生效（不打断正在飞的请求）、按下立刻回显"正在取消…"、零写入。**明确推翻了 `Dedup_AI_Console_PRD.md` 的非目标"不做中途可中断"**，推翻依据写在 PRD 的"与既有文档的冲突"一节。同样没有独立 Eng Design，实现记录与超范围改动写在 PRD 里

## 跨里程碑活文档（归档时的状态快照）

- `Roadmap.md` - 项目路线图原稿；其背景、双层流水线、设计哲学、里程碑总览已吸收进 `docs/SPEC.md`
- `Optimization_Backlog.md` - M1 收尾/M2 开始前的一次优化 review 快照，多数条目已完成，残留观察项在 `Fix_It_Night_Review.md`（本目录）中被交叉引用

## Fix-it Night 评审

- `Fix_It_Night_2026-07_Completion_Report.md` - 2026-07 Fix-it Night 全面评审（基于 commit `8f5af14`）的完成报告：已完成的 22 条 F 编号逐项详情、执行日志、E2E 反馈、以及原始四视角评审分析
- `Fix_It_Night_Review.md` - 上述评审"尚未完成条目"的活跃 backlog 快照（归档于 2026-07-19，短期必须/可修项已全部收口）：未完成清单、逐项分析、P2 收尾批次 + Wave 1 执行记录、以及 F-28/F-41/F-36残留 三条"记为已知边界/不做"的拍板。**中长期低优先级的剩余条目已提炼进活跃的 `docs/Task_Pool.md`**（编号沿用，回溯细节看这里）
- `Fix_It_Agent_2026-07_Completion_Report.md` - 2026-07 agent 层三视角评审（基线 commit `14bee68`）的完成报告：AG- 编号 21 条逐项详情 + 修复记录 + 逐条 commit。已收口并真机验证（agent 测试 252 → 313 全绿，含 C++ headless `pzt delete`）；按设计保留不动的观察项/部署周项已在文内标注

## 代码精简计划

- `Slim_Plan_2026-07-24.md` - 2026-07-24 整仓 slim-plan 分析快照（基线 core 297 / cli 38 doctest、agent 367 pytest）：结论是代码库已相当精简，仅 1 条低风险冗余可执行（T-1 已完成，合并 `agent/compose/adjustment_parser.py` 分类器样板）；`cmd_open` 超长函数（F-2）与两份 scope 解析（F-3）经评估维持不动/待人确认，文内有理由
