# AGENTS.md

## 项目

PicZTream（简称 PZT）是一个基于终端的全键盘图片筛选与色彩处理工具，核心诉求是零延迟选片体验与高性能本地色彩流水线。项目使用现代 C++（C++20+）开发，运行环境固定为 Mac M 系列芯片、Ghostty 终端（Kitty 图像协议）、Tmux 窗格、Fish 4.0、LazyVim、CMake + Ninja 构建链。

## 权威文档

这些文档是本项目设计决策的权威来源，任何代码实现与文档冲突时以文档为准，如需偏离必须先提出并等待确认，不得自行决定。**不要每个 session 全读**，按下述规则加载：

* `docs/SPEC.md`：长期稳定的全局规格（项目定位、模块划分、设计哲学、对外接口轮廓、技术契约、紧凑现状与路线），是每个 session 的 ground truth，**每个 session 唯一必读**，**先读它**
* 其余非 history 文档：先识别当前 session 的开发意图，再按需读取对应文档，不要预先全读：
  * 开发本周 feature → 读 `docs/W{当前周日期}_PRD.md` 及对应 Eng Design。**当前没有活跃周目标**（`W2026-07-21` 已全部收口，归档在 `docs/history/W2026-07-21_*`；上一周 `W2026-07-15` 同样归档在 `docs/history/W2026-07-15_*`，需倒查实现细节时去 history 读）
  * 闲时挑个低优先级活儿修 → 读 `docs/Task_Pool.md`（中长期低优先级任务池，标了 size 与依赖；2026-07 Fix-it Night 的短期必修项已收口，完整评审快照归档在 `docs/history/Fix_It_Night_Review.md`）
  * 其它具体问题 → 查对应的活跃文档（如涉及 RAW 相关改动读 `docs/RAW_Support.md`）
* `docs/history/`：已完成里程碑的 PRD/Eng Design（M0-M4）及被吸收的 `Roadmap.md`/`Optimization_Backlog.md`（见其 `README.md` 索引），最不需要预读，仅在遇到具体设计或开发问题需要倒查某个能力"当初怎么设计的"时再查

具体的工程设计、模块划分、schema 与 tradeoff 以对应周/里程碑的 Eng Design 文档为准，尚未产出对应文档的目标，agent 不应臆测细节自行实现，应先提出问题等待确认。

## 当前状态

M0（MVP）、M1（风格化）、M2（RAW）、M3 的两个增量（选片辅助评分、近似重复检测）、M4 增量一（Telegram 选片-交付闭环，含 agent Style）均已完成。`W2026-07-15` 周目标（含 2026-07-17 追加的目标五 agent 运行时双线程重构）已全部收口并归档：本地模型（Ollama）、recipe/滤镜扩展、agent Style、agent 运行时重构、部署与分发（Homebrew tap 分发 `pzt`/`pzt-agent` + README + 静态主页 + 一键 release 自动化）均完成；**目标二的几何变换（裁切/水平矫正）顺延**，已进 `docs/Task_Pool.md`。`W2026-07-21` 周目标同样已全部收口并归档：eval 解耦（文字描述+硬伤 flag，不再产跨图分数）+ dedup/curate 涉及比较的选择改 AI 锦标赛（整个锦标赛都在 core 的 `tournament` 模块里，分簇/场次推进/判定胜者一次调用做完；PRD 最初设想的"bracket 推进放 agent"在规划阶段就被推翻了，见 `docs/history/W2026-07-21_Tournament_Eng_Design.md` 决策一）+ 全局 AI 开关 + dedup/curate 流程按意图可选化（不再固定全跑）均完成；真机反馈后又追加了一次超出该周 PRD 范围的架构调整（选片确认闸门挪到套滤镜之前，交付不再挂闸门）。手动选片模式明确移出范围，未立项。2026-07-28 的真机反馈又追加了一刀并已完成：控制台补上 `/dedup <范围> --ai`（此前锦标赛只有 agent 层接得上），含开跑前的开销闸门与两段进度反馈，见 `docs/history/Dedup_AI_Console_PRD.md`。2026-07-29 又围绕同一条路径连做了三件事并全部收口：`/dedup` 的两级进度（组 + 每次比较）、Ctrl-C 在信号路径上还原终端（此前会把用户留在备用屏幕里）、以及 Ctrl-C 中途取消这一次去重而不是杀掉整个 `pzt open`（明确推翻了上一份 PRD 的非目标"不做中途可中断"，见 `docs/history/Dedup_Cancel_PRD.md`）。2026-07-30 又收了一条（提案 T-10，归档在 `docs/history/Env_Preflight_*`）：三个运行环境前提（Kitty 协议终端、Telegram 凭证、Ollama 可达）不满足时都不告知原因，现在分别补上终端白名单探测 + 进备用屏幕前的提示与等待、凭证缺失的人话 + 退出码 2、agent 启动期的 Ollama/API key 预检（只告警不拦启动），顺带救活两条被 `DebugLogRedirect` 吞掉的死文案；该条的真机验收推翻了原方案"把提示画进 banner"的做法，理由见归档说明。2026-07-31 再收一条（提案 T-8，归档在 `docs/history/Headless_Observability_*`）：headless 的分钟级 AI 运行在 Telegram 那一侧是纯黑箱，现在进度走 stderr 带外通道（stdout 的原子性不变，SPEC §3.2 契约改述为"stdout 原子 + stderr 带外"）、`pzt_client` 改成两个读取线程边跑边读、`ai_fallback_count` 不再蒸发并进用户话术、`Style`/`StyleApplyAll` 补进可取消集合；真机验收推翻了 PRD"把 phase 压掉"的决策（单位必须跟着数字走）。这一批的来源是 `docs/proposal-2026-07-25.md` 三视角评审，该文档记录了全部 33 条提案条目与逐条完成状态，是当前唯一的活跃待办清单。2026-08-03 又收口了一次**不属于那 33 条的独立立项**：意图驱动的跨簇选片（PRD 与 13 张票已归档在 `docs/history/Intent_Curation_PRD.md` 与 `docs/history/issues/intent-curation/`，文献调研在 `docs/history/Research_Selection_And_VLM_Limits.md`）。`pzt curate` 的第二步此前是空白（开 AI 走 `std::sample` 随机抽样，关 AI 只按拍摄时间散开，用户意图流到这一步只被当字符串打了个标签），现在改为：描述加 `content` 字段、多样性预筛出预选集、curate 内部只评估预选集、由模型按选片简述一次调用连选带排并产出文案，退化时整批落回与关 AI 同一条确定性路径；选片简述在方案确认与选片确认两个阶段都能改；开销闸门与进度按入口分别接线（TUI 阻塞式确认零写入，headless 告知 + 可撤）。这一刀推翻了 `W2026-07-21_PRD.md` 的"agent 不再整批跑评估"（现在只评估预选集）与旧的"视觉/语言推理"分轴（已改述为"关于照片的推理归 core"，见 ADR-0001）。2026-08-08 收口提案 **T-4**（"核心用户是谁"，悬置 26 天）：核心用户 = **有技术背景的摄影爱好者**，能力轴（会用 CLI、能自配 AI 凭证或部署本地模型、能照文档搭 bot）与用途轴（个人旅途照片 → 个人 social media，排除专业/商用/多人协作）两条一起才成立；由此定下 **JPEG 与 HEIC = P0、RAW = P1**（RAW 降级是技术难度所致，不是用户不需要，故不是 P2）。三个下游收敛 2/3：T-27（公开叙事改由"选片成本 = 单次决策延迟 × 决策次数"统领，RAW 旁路降为第三段）与 T-31（Caption **已落地**、是 Curate 的副产品而非第七个 Stage；Profile **不做**；`last_config` 进 Task_Pool）收敛，**T-20 只转为远期开放**（"多用户托管"≠"多人协作"，前者待决、后者不做）。T-5 维持原判，并拍定"HEIC 入口不计入那一半"。拍板同时炸出一个**不在任何清单上的 P0 缺口：HEIC 完全不被支持**（闸门只是 `core/project/project.cpp` 的扩展名白名单，解码/色彩/EXIF 实测零改动即通），另行出 PRD。当前没有活跃周目标。完整现状与路线见 `docs/SPEC.md` 的"现状与路线"一节。

## 工程契约

以下是不随具体业务功能变化的全局约束（`docs/SPEC.md` 第四节是同一套契约的完整版，含依据与展开，两者一致）：

### 架构分层

* `core/`：核心业务逻辑库，不得引入任何终端渲染或按键交互相关的依赖，保持对未来非 `cli` 调用方（如 headless agent 层）的可复用性
* `cli/`：终端交互前端，只调用 `core` 暴露的接口，不承载业务逻辑
* `agent/`：Python headless 编排层，只通过 `pzt` 的 headless 命令（子进程）驱动 `core`，不直接链接 C++
* 新增代码前先判断归属层级，业务逻辑一律进 `core`，交互展示逻辑一律进 `cli`，编排逻辑一律进 `agent`，三者不得相互渗透

### 代码规范

* C++20 及以上标准，禁止引入运行时开销较大的框架或脚本语言依赖
* 并发统一使用 `std::jthread`，禁止裸 `std::thread` 且不管理生命周期
* 不做过早优化，SIMD 等底层优化仅在有实测数据支撑需要时引入
* `core` 层禁止阻塞 IO 到主线程，涉及磁盘读取的路径需评估是否要走异步

### AI 使用边界

LLM（包括 Claude Code 本身）在本项目中的角色严格限定为算法推导、生成结构化配方或配置、代码撰写与审查，不参与任何需要保证确定性和实时性能的核心执行路径，不得在性能关键代码中依赖运行时调用 LLM 进行决策。AI 辅助能力内部再分职责：**关于照片的推理**（看图点评/两两比较/看图选风格/读描述跨簇选片）归 `core`（C++ headless 命令承载），**关于用户的推理**（意图/对话调整/按文字描述匹配风格）归 `agent`（Python）。判据是输入里有没有照片信息，含照片的衍生描述、不限于像素（此前表述为"视觉推理 vs 语言推理"，改述理由见 `docs/adr/0001-core-hosts-photo-reasoning-even-when-text-only.md`）。

### 提交与测试

* 每个功能提交前自查是否覆盖对应 PRD 中列出的验收标准
* 核心逻辑需要基本单元测试覆盖
* 涉及延迟敏感路径的改动，需要有配套的延迟日志或基准数据

### 改动在哪儿落地

沿用 Claude Code 自身的判据（前台交互 session 在 `main` 上直接改，后台/云端 session 开 worktree），不额外区分是否走了 `implement` skill，也不分代码还是文档：

* 后台/云端 session 实现功能、修改代码或文档时，**先开一个 worktree**（`EnterWorktree`），在隔离分支上完成，收口时再合回 `main`

开 worktree 之后**先建一次 release 构建物**，哪怕这次改动根本不碰 C++：

```sh
cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_release
```

理由是 `agent/pzt_client.py` 的 `default_pzt_bin()` 按「`<仓库根>/build_release/cli/pzt` 存在就用它，否则回落到 PATH」解析，而 worktree 里没有构建物（那是 build 产物，不进 git）。回落是**静默**的，于是从 worktree 里起的 agent 会去调 brew 装的那个 `pzt`，版本可能落后好几周，报出来的却是一句看不出所以然的 SQL 错误。真机踩过一次：`no such column: image_evaluations.exposure_score`，那一列在 W2026-07-21 的 eval 解耦里就删了，是 brew 上 2026.7.20 的旧 schema。

`agent/tests/test_pzt_client.py::test_default_pzt_bin_points_at_repo_build_release_cli_pzt` 就是这件事的哨兵：它在 worktree 里红，说的就是"此刻解析到的不是仓库构建物"，别当成环境噪音跳过。

## Agent skills

### Issue tracker

票与 PRD 都在 GitHub Issues（`wangliyangleon/picztream`，用 `gh` CLI）；**ADR 永远留在仓库**，存量 PRD 正在渐进迁移、两处并存是当前的正常状态，找 PRD 前先查已迁清单。见 `docs/agents/issue-tracker.md`。

### Triage labels

沿用五个规范 label（`needs-triage` / `needs-info` / `ready-for-agent` / `ready-for-human` / `wontfix`），全部已存在于仓库。见 `docs/agents/triage-labels.md`。

### Domain docs

单 context：根 `CONTEXT.md` + `docs/adr/`，无 `CONTEXT-MAP.md`。注意 `docs/SPEC.md` 仍是每个 session 唯一必读、且先于这两者。见 `docs/agents/domain.md`。

## 行为准则

Agent 在本仓库中回应时保持客观、严格、简洁、逻辑导向，不做无依据的功能扩展，不引入超出当前里程碑范围的抽象设计，遇到需求不明确或与文档冲突的情况先提出问题，不擅自假设。
