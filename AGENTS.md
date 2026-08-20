# AGENTS.md

本文件只放**最高层、最小概率变化**的东西：项目是什么、怎么分层、硬约束在哪。凡是"当前在做什么、上一批收了什么"一律不进这里，去 `docs/SPEC.md`。

## 项目

PicZTream（简称 PZT）是终端内全键盘的图片筛选与色彩处理工具，核心诉求是零延迟选片体验与高性能本地色彩流水线。

**核心用户**：有技术背景的摄影爱好者。能力轴上他会用 CLI、能自行配置 AI 凭证或部署本地模型、能照文档搭起即时通讯 bot；用途轴上他拍的是自己旅途里的照片，终点是个人 social media。两条轴缺一不可，专业、商用与多人协作明确排除在外。

**核心功能**：秒切浏览与标签分组、recipe 色彩配方、近似重复检测（dedup）、意图驱动的跨簇选片（curate）、导出，以及 Telegram 上「发照片 → 自动去重/选片/套风格 → 推回确认」的闭环。

**格式优先级**：JPEG 与 HEIC 是 P0，RAW 是 P1（opt-in，`--support-raw`）。**HEIC 目前完全不被支持**，是已知的 P0 缺口。

**运行环境固定**：Mac M 系列芯片、Ghostty（Kitty 图像协议，可在 tmux 窗格内渲染物理分辨率大图）、Fish 4.0、LazyVim、CMake + Ninja、现代 C++（C++20+）。

## 设计哲学

- **双层流水线**：culling 阶段只读相机 ISP 已渲染好的内嵌 JPEG 或原生 JPEG，绕过 RAW 解码换取零延迟秒切；processing 阶段只在用户明确标记需要精修时才触发 LibRaw 解码与色彩处理。复杂度按需升级，默认体验永远是轻的。
- **可插拔异步**：所有 AI 能力都能完全关闭而不影响 culling 主流程的零延迟；AI 结果以"建议"形式写回数据库供参考，不直接替代用户判断。
- **不碰用户数据**：原始文件永不被修改，一切状态都在 PZT 自己的库里。
- **自动化后于人工验证**：一个能力先要在人工路径上被用过、调校过，才谈让 agent 自动跑它，否则自动化只是在不稳固的地基上搭黑盒。这条决定了里程碑的推进顺序。
- **不做过早优化、不做超范围抽象**：SIMD 之类的底层优化只在有实测数据支撑时引入；不引入超出当前目标范围的抽象设计。

## 项目地图

依赖方向严格单向：`agent` → `pzt`（cli 二进制）→ `core`，三层不互相渗透。

| 层 | 是什么 | 子模块 |
|---|---|---|
| `core/` | 核心业务逻辑库，不得含任何终端渲染或按键交互依赖 | `project` `db` `decode` `media` `raw` `color` `recipe` `dedup` `curate` `tournament` `tagging` `scope` `export` `ai` `settings` `browse` `api` |
| `cli/` | 终端全键盘交互前端，只调用 `core` 暴露的接口 | `commands` `menu` `ui` `term` `kitty` `text` `i18n` |
| `agent/` | Python headless 编排层，只通过 `pzt --json` 子进程驱动 `core`，不直接链接 C++ | `session`（多线程运行时） `stages` `orchestrator` `compose` `transport` `store` `router` |

- **对外有两个命令面**：面向人的 `pzt` CLI，和面向 agent 的 headless `--json` 命令面。一个能力落在哪一面由**决策归谁**决定，不由它实现在哪里决定；同一个 core 能力可以两面都接。headless 的契约是「stdout 原子 + stderr 带外」：stdout 只在跑完时写一个 JSON 对象，进度与开销走 stderr。
- **agent 三个入口**：`run_telegram.py`（常驻会话，用户主入口）、`run_intent.py`（本地命令行，开发测试）、`run_watchfolder.py`（无对话的全自动回归基线）。Stage 库六个：Ingest / Dedup / Curate / Style / StyleApplyAll / Deliver，由一个零 LLM 决策的确定性 Driver 按 Plan 推进；Plan 本身由一次 LLM 调用从用户意图组装、再经确定性校验。

## 文档

- `docs/SPEC.md` 是长期 ground truth，**每个 session 唯一必读、且先读它**。现状与路线只在那里维护，不要在别处复制一份。
- 其余文档按当前 session 的意图**按需读，不预读**：`docs/Task_Pool.md`（中长期低优先级活儿池）、`docs/RAW_Support.md`（碰 RAW 前必读）、`docs/proposal-2026-07-25.md`（33 条提案与逐条完成状态）、`docs/history/`（已归档的里程碑与周目标，只在倒查"当初怎么设计的"时进）。
- **票与 PRD 在 GitHub Issues**（`wangliyangleon/picztream`，用 `gh` CLI），存量 PRD 渐进迁移中、两处并存是正常状态，找 PRD 前先查已迁清单：`docs/agents/issue-tracker.md`。triage 沿用五个规范 label，见 `docs/agents/triage-labels.md`。
- **ADR 永远留在仓库**（`docs/adr/`），术语表是根 `CONTEXT.md`（只定义概念，不记实现决策），见 `docs/agents/domain.md`。
- 代码实现与权威文档冲突时以文档为准，要偏离必须先提出并等待确认。尚未产出对应文档的目标，不臆测细节自行实现。

## 工程契约

`docs/SPEC.md` 第四节是同一套契约的完整版（含依据与展开），两者一致。

**分层**：新增代码前先判归属 - 业务逻辑一律进 `core`，交互展示一律进 `cli`，编排一律进 `agent`。

**代码规范**：C++20 及以上；并发统一 `std::jthread`，禁止裸 `std::thread` 且不管理生命周期；`core` 层禁止阻塞 IO 到主线程；禁止引入运行时开销较大的框架或脚本语言依赖。

**AI 边界**：LLM（包括 Claude Code 本身）只做算法推导、生成结构化配方或配置、代码撰写与审查，不进任何需要确定性与实时性能的核心执行路径。AI 能力内部再分职责：**关于照片的推理归 `core`**（看图点评、两两比较、看图选风格、读描述跨簇选片），**关于用户的推理归 `agent`**（解析意图、解析对话调整、按文字描述匹配风格）。判据是输入里有没有照片信息，含照片的衍生描述、不限于像素，见 `docs/adr/0001-core-hosts-photo-reasoning-even-when-text-only.md`。

**提交与测试**：提交前自查是否覆盖对应 PRD 的验收标准；核心逻辑要有单元测试覆盖，遵循 TDD 节奏（先 RED 后 GREEN，一个可提交单元一次 commit）；涉及延迟敏感路径的改动要有配套延迟日志或基准数据。改了 C++ 之后 `build/` 与 `build_release/` 两个目录都要重建，用户是拿 release 二进制实测的。

**worktree 里的两个坑**：

1. 开 worktree 之后**先建一次 release 构建物**，哪怕这次改动根本不碰 C++：

   ```sh
   cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build_release
   ```

   `agent/pzt_client.py` 的 `default_pzt_bin()` 按「仓库根的 `build_release/cli/pzt` 存在就用它，否则回落到 PATH」解析，而构建物不进 git、worktree 里没有。回落是**静默**的，于是从 worktree 里起的 agent 会去调 brew 装的那个可能落后好几周的 `pzt`，症状却是一句看不出所以然的 SQL 错误。`agent/tests/test_pzt_client.py::test_default_pzt_bin_points_at_repo_build_release_cli_pzt` 是这件事的哨兵，它在 worktree 里红说的就是"此刻解析到的不是仓库构建物"，别当环境噪音跳过。

2. `agent/.venv` 只存在于主 checkout，worktree 里没有（venv 不进 git），且这台机器上没有 `python` 这个命令。在 worktree 的 `agent/` 目录里要这样跑测试：

   ```sh
   /Users/wangliyang/Dev/picztream/agent/.venv/bin/python -m pytest tests/
   ```

   `pyproject.toml` 的 `pythonpath = ["."]` 保证 import 解析到当前 worktree 的代码，测的仍是本次改动。

## 行为准则

回应保持客观、严格、简洁、逻辑导向。不做无依据的功能扩展，不引入超出当前范围的抽象设计。遇到需求不明确或与文档冲突的情况先提出问题、等待确认，不擅自假设。
