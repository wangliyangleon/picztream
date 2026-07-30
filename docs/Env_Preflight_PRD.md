# PZT 产品需求文档：环境前提不满足时告知原因

> 来源：`docs/proposal-2026-07-25.md` 的 **T-10**（User 视角 U-11，P1，无依赖）。
>
> 本文档只描述需求与 deliverable，不含 schema/类/依赖级别的实现方案，那部分进对应的 Eng Design。

## 背景与问题陈述

PZT 有三个必须满足的运行环境前提：终端讲 Kitty 图像协议、Telegram 凭证齐备、Ollama 可达。三个前提各自不满足时程序都会失败，但**没有一处告诉用户失败的原因是这个前提**。

三种失败形态，按恶劣程度排序：

| | 失败形态 | 用户看到的 | 能否自诊断 |
|---|---|---|---|
| (a) 非 Kitty 终端 | 静默成功 | 边框、信息栏、按键菜单全都画好，只有图片区永远空白 | 不能 |
| (b) 缺 Telegram 凭证 | 裸崩 | Python traceback | 勉强 |
| (c) Ollama 未启动 | 延迟且模糊 | 发完照片、打完意图之后才收到"AI 服务好像连不上" | 不能 |

(a) 是最严重的一条：**产品的核心卖点静默失效**。`docs/SPEC.md` §1 把"终端内零延迟秒切浏览"写成两个核心差异点之一，而在 iTerm2 / Terminal.app / Alacritty 里，这个卖点表现为一个功能完整、只是不显示照片的界面，用户没有任何线索指向"我的终端不对"。

### (a) 完全没有探测 Kitty 图像协议支持

`cli/kitty/kitty.cpp:95-111` 的 `detect_terminal_mode()` 只做一件事：判断在不在 tmux 里，在的话查一次 `tmux show-options -gqv allow-passthrough`。产出的 `TerminalMode`（`kitty.h:52-55`）只有 `inside_tmux` / `passthrough_ok` 两个 bool。全仓没有任何 Kitty 协议探针（`a=q` 查询），也不看 `TERM` / `TERM_PROGRAM`。

失败为什么是静默的，看 `render_rgba_via_tmpfile` 的错误面（`kitty.h:60-63`）：`RenderError` 只有 `PassthroughDisabled` 和 `WriteFailed` 两个值。在 iTerm2 里，图片转义序列被 `write()` 正常写出、返回值等于长度，函数返回 `Ok`，终端把不认识的 APC 序列丢掉。**在整条链路上，这次渲染是成功的。**

雪上加霜的是控制序列带 `q=2`（`kitty.h` 注释记录了原因：让终端永不回发响应，否则会被全键盘循环当按键消费，历史上导致过一次真实死循环）。这个决定本身是对的，但副作用是**主动放弃了唯一的被动反馈通道**。

### (a-2) 同一族的第四处缺陷（不在提案原文里，本 PRD 纳入范围）

即便渲染真的返回了 `RenderError`，`browse.cpp:1285-1287` 把提示打到 **stderr**，而同一函数在 `browse.cpp:798-813` 用 `DebugLogRedirect` **默认把整个 stderr 丢掉**（不开 `--debug` 时直接扔）。因此 `err_open_render_failed` 这条文案在默认路径上是**永远看不见的死文案**，`err_open_decode_failed`（`:1289`）同理。

这是"失败不告知原因"的同一族缺陷，且与 (a) 共用同一个展示载体，一并处理。

### (a-3) 已有的正确范本

tmux passthrough 那条**做对了**：`browse.cpp:761-766` 在进 AltScreen 之前预检，不满足就 `fprintf(stderr)` + `return 1`，文案（`i18n.cpp:724-735`）直接给出修复命令 `set -g allow-passthrough on`，并给出备选路径（在独立 Ghostty 窗口里跑）。

这是一个 fail-fast + 可操作提示的完整范本。**本增量本质上是把另外三处拉到这条已经存在的模式上**，不发明新机制。

### (b) `token_from_env` 在任何 try 之外

`agent/transport/telegram_client.py:33-44` 本身做得不差：抛的是**带类型的** `TelegramConfigError("missing_token", ...)` / `("missing_chat_id", ...)`，不是裸 `KeyError`。

问题纯在调用点：`agent/run_telegram.py:135-136` 的两行裸调用外面没有任何 try，于是这个本来可以被翻译成人话的类型化错误直接以 traceback 形式糊在屏幕上。错误分类机制已经现成，**这一处基本是接线**。

范围只涉及 `run_telegram.py`：`run_intent.py` / `run_watchfolder.py` 都不用这两个 helper（已复核）。

### (c) 没有 Ollama 可达性预检

Ollama 有**两个消费者，分属两层，各有一份配置真相源**：

- 语言推理：`agent/compose/llm_client.py:23-24` 硬编码 `_OLLAMA_BASE_URL = "http://localhost:11434"` / `_OLLAMA_MODEL = "gemma4:e2b"`
- 视觉推理：`core/settings/settings.h:25-26` 的 `ollama_base_url` / `ollama_model`（可经 `config.json` 覆盖）

两处都是"用到了才连"。`agent/session/consumer.py:57` 已有 `_looks_like_infra_error()`，`_on_compose_failed`（`:857-863`）也已能把基础设施故障和"没听懂意图"区分开，会说"AI 服务好像连不上"。**所以缺的不是错误分类，是两件事**：

1. **时机**：要等用户传完照片、打完意图，编排失败了才知道
2. **可操作性**：说了连不上，没说去 `ollama serve`，也没说是哪个 URL、哪个模型没 pull

"服务没起"与"模型没 pull"是两种不同的失败，需要区别告知。

### (c-2) 云端 provider 的 API key 是同一种延迟失败（本 PRD 纳入范围）

`llm_client.py:_get_api_key` 在 `provider` 为 `claude`/`gemini` 时才读 `ANTHROPIC_API_KEY`/`GEMINI_API_KEY`，缺了抛 `LlmRequestError("missing_api_key", ...)`。而 `meta_provider` 在 `run_telegram.py:56` 启动时就读定了，**启动时已经知道要用哪个 provider，却要等到第一次真实调用才发现 key 没配**。与 (c) 同一形态、同一修法，成本极低，一并做。

### 探测手段的关键约束（影响 (a) 的可靠性，必须记档）

实测本机环境（Ghostty + tmux，即项目的主目标环境）：

```
pane 内：  TERM=screen-256color   TERM_PROGRAM=tmux        GHOSTTY_BIN_DIR=/Applications/Ghostty.app/...
tmux -g：  TERM=xterm-ghostty     TERM_PROGRAM=ghostty     __CFBundleIdentifier=com.mitchellh.ghostty
update-environment：不含 TERM_PROGRAM，也不含任何 GHOSTTY_*
```

三条结论：

1. **tmux 里 Ghostty 的身份在 pane 环境里被擦掉了**：`TERM_PROGRAM` 变成 `tmux`、`TERM` 变成 `screen-256color`。不在 tmux 里时两者都干净可靠（`xterm-ghostty` / `ghostty`）。
2. tmux 内唯一残留的信号是 `GHOSTTY_*` 系列（由 Ghostty 的 shell integration 注入，被 tmux server 的初始环境继承进新 pane）。
3. 但 `update-environment` 不含它们，意味着**换终端 attach 之后这些值是 stale 的**：从 Ghostty 起的 server 后来用 iTerm2 attach，新 pane 里仍然看得到 `GHOSTTY_*`。

也就是说，**白名单探测在项目的主目标环境（Ghostty + tmux）下恰好是最不可靠的**。两种误判方向及后果：

- **假阴性**（实际支持，判成不支持）：多出一行提示。可经配置关掉。
- **假阳性**（实际不支持，判成支持）：退回现状的静默空白，**不构成相对今天的回退**。

这个不对称是选 C 方案（仅警告、不阻止）的实质理由：假阳性最坏等于现状，假阴性只是噪音，两个方向都不会把用户挡在门外。

## 目标（本增量范围）

* **(a) `pzt open` 在终端可能不支持 Kitty 图像协议时给出提示**，并指名修复动作（装 Ghostty），但**不阻止进入**
  * 提示要出现在用户实际会看的地方，且不能被 `DebugLogRedirect` 吞掉
  * 提示可经 `config.json` 关掉（给假阴性用户的逃生口）
  * 探测逻辑留在 `cli/kitty`，不下沉 `core`（`kitty.h:11-14` 引 M0 Eng Design 的既有约束）
* **(a-2) 渲染失败与解码失败的既有提示从被丢弃的 stderr 挪到用户可见处**，让两条死文案真正生效
* **(b) `pzt-agent` 缺 Telegram 凭证时给一句人话并非零退出**，不抛 traceback
  * 按 `missing_token` / `missing_chat_id` 两个 code 分别指名要设哪个环境变量
* **(c) agent 启动时对 Ollama 做一次可达性预检，失败仅警告、不拒绝启动**
  * 区分"服务不可达"与"模型未 pull"两种失败，各自给出对应动作（`ollama serve` / `ollama pull <model>`）
  * 提示里带上实际用的 URL 与模型名，不让用户猜
* **(c-2) `meta_provider` 为云端时，启动时预检对应 API key 是否存在**，缺失仅警告

## 非目标

* **不做主动协议查询**（A 方案）：不发 `a=q` 探针、不在 raw 模式下带超时读 stdin。理由见"已拍板的设计决策"第 1 条
* **不改 `q=2`**：终端永不回发响应这条约定不动，它挡掉过一次真实死循环
* **不做重试、不做后台轮询、不做健康状态常驻显示**：(c) 只解决"用户不知道原因"，不解决"服务不可用"
* **不改 core 的 AI 调用路径与降级行为**：`ai_fallback_count`、超时退化这些是 T-8 的地盘，本增量不碰
* **不给 core 加同步阻塞探活**：`core` 禁止主线程阻塞 IO（SPEC §4.2）。(c) 的预检落在 agent 启动路径上
* **不做终端能力的运行时重新探测**：一次会话探一次，跟 `detect_terminal_mode()` 现有语义一致
* **不动 headless 命令面**：`--json` 系列的参数、输出、退出码全部不变

## 已拍板的设计决策

1. **(a) 走 C 方案：环境变量白名单打底，不在白名单时给非阻塞提示，不阻止进入。**
   否决 A（主动 `a=q` 查询）的理由有三：要在 raw 模式下做一次带 timeout 的 stdin 读，与全键盘主循环的输入路径抢同一个 fd；tmux 下响应还要穿透 DCS 包裹；且与 `q=2` 的"终端永不回发"约定直接冲突，而那条约定是为修一个真实死循环立的。否决 B（纯白名单、判定即拦）的理由是白名单必然漏判新终端，在主目标环境下还会因 tmux 环境 stale 而假阴性，拦人的代价过高。

2. **(c) 探活失败仅警告，不拒绝启动。**
   常驻会话不该因为一个稍后可能被起来的服务而拒绝拉起。用户完全可能先起 agent 再起 Ollama。

3. **(a) 的提示同时出现在两处：TUI banner 第一帧 + 退出后的真实终端。**
   单靠"进 AltScreen 之前打一行"是无效的：AltScreen 立刻切到备用缓冲区，那一行当场被盖掉。banner 是现成载体（`/dedup` 的进度、`status_override` 都画在那儿），且文本渲染在任何终端上都正常，正是这类用户唯一能看见的通道。退出后在真实终端上再打一次，保证用户即使错过第一帧也能拿到信息（`browse.cpp` 已有"块结束、stderr 换回真实终端之后再打印退出汇总"的先例）。

4. **(a) 的判定是"可能不支持"，不是"不支持"。**
   文案措辞必须反映白名单的不确定性，不能断言用户的终端不行。

5. **(a-2) 复用同一个 banner 载体**，不为渲染/解码失败另造展示机制。

6. **探测函数按纯函数设计，环境变量由调用方注入。**
   跟 `parse_allow_passthrough` 同一个先例（`kitty.h:41-45` 明确记录了"抽成纯函数方便单元测试，不需要真的起一个 tmux 会话"），保证 (a) 的判定逻辑可以在不起真实终端的情况下单测。

## 验收标准

**(a) 终端探测**

* 在 Ghostty 里（tmux 内、tmux 外各一次）跑 `pzt open`：**不出现**任何终端相关提示，行为与今天完全一致
* 在 iTerm2 或 Terminal.app 里跑 `pzt open`：banner 第一帧出现提示，指名装 Ghostty；退出后真实终端上再见到一次
* 提示不阻塞：不需要按任何键确认，`h`/`l` 等按键行为不变
* `config.json` 里关掉之后，上一条的提示不再出现
* 判定逻辑有单测覆盖，至少覆盖：tmux 外的 Ghostty、tmux 内的 Ghostty（只有 `GHOSTTY_*` 可依据）、tmux 外的非白名单终端、两者都缺的裸环境
* 既有的 tmux passthrough 预检行为不变（仍然是 fail-fast + `return 1`）

**(a-2) 渲染/解码失败可见**

* 构造一次渲染失败（如在 tmux 内关掉 allow-passthrough 后绕过预检，或注入一次写失败）：不开 `--debug` 也能在界面上看到提示
* 开 `--debug` 时 debug 面板行为不变

**(b) Telegram 凭证**

* 不设 `TELEGRAM_BOT_TOKEN` 跑 `pzt-agent`：输出一句指名该变量的人话，**无 traceback**，退出码非零
* 只缺 `TELEGRAM_CHAT_ID` 时同理，且提示指向的是 chat id 而不是 token
* 两个都齐备时启动路径不变

**(c) Ollama 预检**

* 停掉 Ollama 后启动 agent：**启动成功**，同时输出一句提示，含实际 URL 与"去 `ollama serve`"
* Ollama 起着但模型没 pull：提示指向 `ollama pull <实际模型名>`，与上一条可区分
* Ollama 正常：无额外输出
* 预检失败后，用户随后起好 Ollama，本次会话仍能正常工作（预检不缓存"不可用"结论去挡后续调用）

**(c-2) 云端 key**

* `PZT_AGENT_META_PROVIDER=claude` 且 `ANTHROPIC_API_KEY` 未设：启动时就给出提示（不是等第一次意图解析），且仍然启动成功

**回归**

* `core` / `cli` / agent 三侧既有测试全绿
* Debug 与 Release 两个 build 目录都重新编过

## 风险与已接受的代价

1. **白名单假阳性**：从 Ghostty 起的 tmux server 后来用 iTerm2 attach，`GHOSTTY_*` 是 stale 的，会判成支持而不提示。**已接受**：此情形退回今天的静默空白，不构成回退。真机踩到再评估是否加 `tmux show-environment -g` 的交叉校验。
2. **白名单假阴性**：不在白名单的终端（包括未来支持 Kitty 协议的新终端）会收到不必要的提示。**已接受**：代价是一行噪音，且有配置开关可关。
3. **两份 Ollama 配置真相源**：agent 侧硬编码、core 侧走 `Settings`。本增量的预检只覆盖 agent 侧自己用的那份，**不统一这两处配置**（那是独立的收敛任务，超出本范围）。若用户改了 `config.json` 的 `ollama_base_url` 而 agent 侧仍探 `localhost:11434`，预检结论对视觉推理路径不适用。这个不一致是既有事实，本 PRD 只记档、不修。
4. **banner 承载的提示会被后续操作覆盖**：第一帧之后用户一按键就没了。由决策 3 的"退出后再打一次"兜底。

## 与既有文档的关系

* 不推翻任何已归档的非目标。
* `kitty.h:11-14` 的"终端探测细节只能待在 `cli/kitty`、不得下沉 `core`"（引 M0 Eng Design）在本增量中继续遵守。
* 符合提案 §5 的 T-5 配比约束：主体 deliverable 落在 `cli/` 与 `agent/` 的人可见路径。
* 完成后本文档归档进 `docs/history/`，并回填 `docs/proposal-2026-07-25.md` 的 T-10 状态。
