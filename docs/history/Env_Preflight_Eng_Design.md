# PZT 工程设计：环境前提不满足时告知原因

> **归档说明（2026-07-30）**：各段已全部实现。有两处偏离，**其中决策三被真机验收推翻**：
>
> - **决策三的"提示画进 banner"作废**（commit `4b2cb89`）。两个陷阱（吃按键、渲染失败每帧复发）在实现中都确认存在，判断没错；错的是它们之外的一个未言明前提——"banner 是文本，任何终端都画得出来"。文本本身确实画得出，但 banner 画在图片序列**之前**，不认识 Kitty 协议的终端会把紧随其后的 APC 序列整段当普通文本打出来，几百字节 base64 一换行就把画面顶上去，banner 连同边框一起被滚掉。**恰恰在这条提示最该出现的终端里，banner 是最不可能被看见的位置。** 改为进备用屏幕之前打在真实终端上并等一次按键。
>   - notice 通道本身**保留**：B.1 的渲染/解码失败仍然走它，那两条在正常终端（Ghostty）下 banner 是可靠的。
>   - `warn_terminal_banner()` 随之成为无消费者的死代码，已删除；它那条 52 列宽度断言改挂到 B.1 那两条仍走 banner 的文案上，约束没丢。
>
> 另一处是实现期自己发现的：
>
> - **文案从一条拆成两条**。写这份文档时 §6 只规划了一条终端提示。实现 A.2 时量了才发现塞不下：banner 那行的可用宽度是 `content_cols = 终端宽 × ui_width_ratio(0.7) - 2`，80 列终端上只有 54 列，而 `pad_to` 超宽是**静默 truncate**；完整文案 zh 168 列 / en 230 列，截断后活下来的是前半句，被砍掉的恰好是最后那句"怎么关掉"，也就是假阴性用户最需要的那半句。于是拆成 banner 版（只说结论，zh 39 / en 45 列）与 detail 版（退出后打在真实终端上，能自然折行）。测试里把 52 列这个上限直接断言住。
>
> 决策六（`GET /api/tags`）的响应格式假设已对真实本地 Ollama 验证过。
>
> **方法论教训**：决策三之所以能带着一个错误前提走到真机，是因为 pty 模拟只验证了"字节有没有按预期写出去"（banner 确实写在 offset 1772），没验证也验证不了"终端拿这些字节怎么办"。凡是结论依赖终端行为的，模拟不能替代真机。
>
> 实现提交清单见 `Env_Preflight_PRD.md` 的归档说明。
>
> 对应 PRD：`docs/history/Env_Preflight_PRD.md`（提案 T-10）。本文档只承载实现方案、模块划分与 tradeoff，需求与验收标准以 PRD 为准。

## 一、范围

本文档覆盖 PRD 剩余的四段：

| 段 | PRD 目标 | 层 |
|---|---|---|
| A | (a) 终端可能不支持 Kitty 协议时给非阻塞提示 | `cli/kitty` + `cli/commands` + `core/settings` + `cli/i18n` |
| B | (a-2) 渲染/解码失败的死文案复活 | `cli/commands` |
| C | (b) Telegram 凭证 **已完成** | `agent` |
| D | (c) Ollama 预检 + (c-2) 云端 key 预检 | `agent/compose` + `agent/run_telegram.py` |

`core` 除了 `Settings` 加一个字段之外零改动。视觉推理侧（`core/ai`）的 Ollama 不在本增量内，理由见 PRD 风险 3。

## 二、决策一：白名单具体内容

PRD 只定了走 C 方案（白名单 + 非阻塞提示），没定收哪些。这里定下来：

| 条件 | 判定 | 依据 |
|---|---|---|
| `TERM_PROGRAM == "ghostty"` | 命中 | 主目标环境，tmux 外实测可靠 |
| `TERM` 以 `xterm-ghostty` 开头 | 命中 | 同上，Ghostty 自带 terminfo |
| `TERM` 以 `xterm-kitty` 开头 | 命中 | 协议的定义方，`TERM` 是它的标准标识 |
| `KITTY_WINDOW_ID` 存在 | 命中 | kitty 自注入，tmux 内也会漏进来 |
| `GHOSTTY_RESOURCES_DIR` 或 `GHOSTTY_BIN_DIR` 存在 | 命中 | **tmux 内唯一可用的信号**，见决策二 |
| 以上都不命中 | 判"可能不支持"，给提示 | |

**不收 WezTerm**：它对 Kitty 图像协议的支持是部分的（不同版本对 `t=t` 临时文件介质与 `q=2` 的行为不一致），而本项目从未在它上面实测过。收进来等于承诺一个没验证过的环境，比漏判更糟。漏判的代价只是一行可关掉的提示。

**判定顺序无关**：任一条命中即命中，不做优先级。

## 三、决策二：`TerminalMode` 的形状与探测函数签名

`TerminalMode`（`cli/kitty/kitty.h:52-55`）加**一个 bool**，不加枚举：

```cpp
struct TerminalMode {
  bool inside_tmux = false;
  bool passthrough_ok = true;
  // 白名单命中 = true。默认 true(静默)是刻意的:非交互调用方(pzt render)
  // 和单测不该因为没探测就收到提示。
  bool kitty_support_likely = true;
};
```

默认 `true` 的理由：这是"是否要提示"的开关，不是"终端能不能用"的结论。默认静默，只有真的探测过且没命中才转 `false`，跟 `passthrough_ok` 默认 `true` 的既有语义一致（不在 tmux 里时无所谓那个开关）。

探测逻辑抽成纯函数，环境变量由调用方注入：

```cpp
// 环境变量以取值函数注入，单测不需要 setenv/unsetenv 污染进程环境。
// 照 parse_allow_passthrough 的先例(kitty.h:41-45)。
using EnvLookupFn = std::function<std::optional<std::string>(std::string_view)>;
bool kitty_support_likely(const EnvLookupFn& lookup, bool inside_tmux);
```

`inside_tmux` 显式传入而不是在函数内部再探一次：`detect_terminal_mode()` 已经算过，且单测要能独立构造 tmux 内/外两种情形。

**tmux 内的降级规则**：`inside_tmux == true` 时，`TERM_PROGRAM` 与 `TERM` 两条**直接跳过不看**。实测这两个值在 pane 里分别是 `tmux` 和 `screen-256color`，Ghostty 的身份被擦掉了（PRD"探测手段的关键约束"一节），看它们只会稳定误判。tmux 内只认 `GHOSTTY_*` 与 `KITTY_WINDOW_ID`。

## 四、决策三：banner 提示的载体（本段最实质的设计）

> **已被真机验收推翻**，见文档顶部归档说明。下面保留原文：两个陷阱的判断是对的、notice 通道也保留给了 B.1，错的是"banner 在任何终端都看得见"这个未言明的前提。终端提示已改为进备用屏幕之前打 + 等一次按键。

PRD 决策 3 只说了"banner 第一帧 + 退出后真实终端"。看完代码发现**不能复用 `status_override`**，有两个陷阱：

1. **它会吃掉一次按键**。`browse.cpp:1192-1209` 里 `status_override` 非空时走 `msg_press_any_key_to_continue()` 并置 `showing_status = true`，而 `showing_status` 的语义（`browse.cpp:860-866` 注释）是"下一次读键不管读到什么都只用来消除提示，不当成具体动作"。这与 PRD 验收标准里"提示不阻塞，不需要按任何键确认，`h`/`l` 等按键行为不变"直接冲突。
2. **用在 B 段上会把用户锁死**。渲染失败不是一次性事件，终端不对时它**每帧都会失败**。如果每帧都置 `status_override`，那么每次按键都只用于消除提示、然后重画又失败、又置上，用户**永远无法导航**。这是个比原缺陷更严重的回归。

因此新增一个**一次性 session notice 通道**，A 段和 B 段共用（这正是 PRD 决策 5 说的"复用同一个 banner 载体"）：

- `cmd_open` 作用域内一个 `std::vector<std::string> pending_notices`，与 `status_override` 是**平行的、互不干扰的**两套：notice 不置 `showing_status`、不拼"按任意键继续"、不吃按键。
- 有 pending notice 的那一帧，**banner 第二行**临时换成 notice 内容（取代 `nav_bar_line2()`），画完即弹出。选第二行是因为第一行的 `nav_bar_line1()` 承载 `h`/`l`/`j`/`k`/`q` 这些随时要看的导航键，占掉代价更大。
- **B 段按"种类"去重**：渲染失败、解码失败各自一个 session 内的 `bool` 闸门，只入队一次。既解决陷阱 2，也避免同一句话刷屏。
- 退出后在真实终端上把本次会话所有 notice 再打一遍。落点必须在 `DebugLogRedirect` 析构**之后**（`browse.cpp:798-813` 的注释已经踩过这个坑：那个对象活着的时候 stderr 是被吞的），照打延迟汇总的既有先例。

**已知代价**：一次性 notice 在用户按第一个键之后就没了，比"按任意键继续"更容易错过。退出后重打是兜底。如果真机验收发现还是容易漏，改成用 `status_override` 是一行的事（A 段是一次性事件，不受陷阱 2 影响），但那要以放弃 PRD 的"不阻塞"验收标准为代价，属于需要重新拍板的偏离，不在实现期自行决定。

## 五、决策四：配置开关放 `core::Settings`

字段 `bool warn_unsupported_terminal = true;`，`config.json` 同名键，`settings.cpp` 加一行 `assign_if_present`。

放 `core` 而不是 cli 侧另起配置：`ui_width_ratio`、`prefetch_window`、`lang` 三个同样是纯 cli 关切的字段都在 `core::Settings` 里，这是既有先例（`settings.h` 头部注释把"界面偏好"明确列入职责）。为一个 bool 另开一套配置文件是超范围抽象。

## 六、决策五：i18n 文案

> **实现期更新**：原计划一条，实测放不下，改成两条。理由见本文档顶部归档说明。

`cli/i18n` 新增**两条**，各 zh/en 双份，措辞按 PRD 决策 4 用"可能不支持"：

- `warn_terminal_banner()`：只说结论（"当前终端可能不支持 Kitty 图像协议"）。必须塞得进最窄常见终端的 banner 宽度（80 列终端上 54 列），测试断言 ≤ 52 列。
- `warn_terminal_detail()`：完整版，含"图片区会是空的""建议改用 Ghostty""把 `config.json` 的 `warn_unsupported_terminal` 设成 false 可以关掉这句"。由退出后打在真实终端上的那一次承载，不受 banner 宽度限制。

最后那半句是必须的：假阴性用户需要知道逃生口在哪，否则这行提示对他们就是永久噪音。**它只存在于 detail 版**，这正是不能只留一条长文案的原因。

两条都不带结尾换行，换行由调用方按场景补。

`err_open_render_failed()` / `err_open_decode_failed()` 两条**文案本身不动**，只改投递路径（进 notice 通道）。

`err_open_render_failed()` / `err_open_decode_failed()` 两条**文案本身不动**，只改投递路径（进 notice 通道）。

## 七、决策六：Ollama 预检用 `GET /api/tags` 一次调用区分两种失败

PRD 要求区分"服务不可达"与"模型未 pull"。不需要两次探测：

| 观察 | 结论 | 提示动作 |
|---|---|---|
| 连接层失败（`URLError`） | 服务不可达 | `ollama serve` |
| 200，模型在返回列表里 | 正常 | 静默 |
| 200，模型不在列表里 | 未 pull | `ollama pull <实际模型名>` |
| 200，body 解析不出来 | **不告警** | - |

最后一行是刻意的：拿到 200 说明服务是活的，模型清单解析失败属于 PZT 自己对 Ollama 响应格式的假设过期，不是用户的环境问题。为此告警是把我们的 bug 报成用户的错。这条会在实现里留注释说明。

**不用 POST `/api/chat` 探活**：那会真的加载模型、耗时到秒级甚至十几秒，把"启动"变慢，而我们只需要知道服务在不在、模型有没有。

**有效模型名必须按 `PZT_AGENT_OLLAMA_MODEL` 解析**，不能直接读 `_OLLAMA_MODEL` 常量：`llm_client.py:156` 的真实调用走的是 `os.environ.get("PZT_AGENT_OLLAMA_MODEL", _OLLAMA_MODEL)`。预检若只看常量，用户覆盖过模型名时会报错一个他根本没在用的模型。这里抽一个 `effective_ollama_model()` 供两处共用，消掉这个隐患而不是复制它。

## 八、决策七：预检模块归属与注入接缝

新模块 **`agent/compose/preflight.py`**，不新建顶层模块。

两个理由：

1. **打包白名单**。`pyproject.toml` 的 `py-modules = ["run_telegram", "pzt_client", "log_setup"]` 是显式白名单，新增顶层模块**必须同时改这一行**才会进 wheel，漏了就是"本地跑得通、`brew install` 之后 ImportError"。放进 `compose*` 由 `packages.find` 自动覆盖，无打包改动。
2. **配置真相源在 compose**。`_OLLAMA_BASE_URL` / `_OLLAMA_MODEL` / 两个 API key 的环境变量名都是 `compose/llm_client.py` 的私有常量。预检放旁边可以直接复用，放别处就得把它们再抄一遍，那正是本增量在别处要消掉的问题。

注入接缝镜像 `llm_client` 的 `HttpPostFn`：

```python
HttpGetFn = Callable[[str], Tuple[int, str]]   # url -> (status, body)
```

需要新加是因为 `HttpPostFn` 是 POST 专用（签名带 headers 与 body），而 `/api/tags` 是 GET。真实实现 `_real_http_get` 照 `_real_http_post`（`llm_client.py:81-89`）写，**超时必须短**（拟 2 秒）：这是启动路径上的探活，不是业务调用，60 秒超时会把"Ollama 没起"变成"agent 卡半分钟才启动"。

预检函数返回结构化结果而不是直接打日志：

```python
# 返回 (code, hint) 或 None(一切正常)。code 供测试断言，hint 是给人看的那句话。
def check_ollama(base_url, model, http_get=None) -> Optional[Tuple[str, str]]
def check_meta_provider_key(provider) -> Optional[Tuple[str, str]]
```

理由跟 C.1 里 `config_error_hint()` 一样：判定与展示分开，测试断 code、人读 hint。日志由调用方 `run_telegram.py` 打。

## 九、决策八：provider 解析要挪到 `main()` 能看见的地方

(c-2) 要在启动时按 `meta_provider` 决定查哪个 key，但那个值现在读在 `build_runtime()` 里（`run_telegram.py:77`），`main()` 拿不到。

处置：把这段解析抽成 `resolve_meta_provider()`，`main()` 与 `build_runtime()` 都调它。**不改 `build_runtime()` 的签名**，避免动到 `tests/session/test_run_telegram.py` 里已有的三个接线测试（其中两个专门锁 `PZT_AGENT_META_PROVIDER` 的读取与校验行为）。

## 十、顺带发现的第四处同族缺陷（需要你拍是否纳入）

`run_telegram.py:78-79`：`PZT_AGENT_META_PROVIDER` 非法时 `raise ValueError(...)`，而这一句在 `main()` 里同样没有 try 包着。**这是与 C.1 完全相同的形态**：启动前提不对 → 裸 traceback，且 `pzt-agent` 的 console_script 直接调 `main()`。

它不在 PRD 的目标列表里（PRD 的 (b) 只写了 Telegram 凭证）。但决策八已经要碰这段代码，顺手把它接进 C.1 那个既有的 except 分支只是几行。

**已拍板：纳入**（2026-07-30）。在 D 段随决策八一起做：`resolve_meta_provider()` 抽出来之后，非法值改成走 C.1 那条既有的 except 分支，同样是一句人话 + 退出码 2，不再裸抛 `ValueError`。

## 十一、文件改动清单

| 文件 | 改动 | 段 |
|---|---|---|
| `cli/kitty/kitty.h` | `TerminalMode` 加字段；声明 `EnvLookupFn` / `kitty_support_likely()` | A.1 |
| `cli/kitty/kitty.cpp` | 实现纯函数；`detect_terminal_mode()` 里填字段 | A.1 |
| `cli/tests/kitty_test.cpp` | 白名单表驱动单测 | A.1 |
| `core/settings/settings.h` / `.cpp` | `warn_unsupported_terminal` 字段 + 解析 | A.2 |
| `core/tests/settings_test.cpp` | 默认值 + 解析两个 case | A.2 |
| `cli/i18n/i18n.h` / `.cpp` | 新增终端提示文案（zh/en） | A.2 |
| `cli/commands/browse.cpp` | notice 通道；终端提示入队；退出后重打；渲染/解码失败改投递 | A.3 / B.1 |
| `agent/compose/llm_client.py` | 抽 `effective_ollama_model()`，暴露 base_url / key 环境变量名 | D.1 |
| `agent/compose/preflight.py` | 新增：`check_ollama` / `check_meta_provider_key` / `_real_http_get` | D.1 |
| `agent/tests/compose/test_preflight.py` | 注入 `HttpGetFn` 的四种情形 + key 预检 | D.1 |
| `agent/run_telegram.py` | `resolve_meta_provider()`；启动期调预检、仅告警 | D.2 / D.3 |

## 十二、测试策略与覆盖边界

**能单测的**：A.1 的白名单判定（纯函数 + 注入 env，覆盖 PRD 列的四种环境）、A.2 的配置解析、D.1 的四种 Ollama 情形与 key 预检。这些都能走严格 RED → GREEN。

**不能单测的**：A.3 与 B.1 全部落在 `cmd_open` 那个 950 行函数里，它零测试覆盖（提案 A-1/A-2 记的正是这条债），本增量**不顺手补**那层。这两段的正确性由 PRD 的真机验收（§E.2）覆盖，其中 B.1 需要构造一次真实的渲染失败（在 tmux 内关掉 `allow-passthrough`，绕过预检直接进主循环）。

这是本增量最大的一处覆盖缺口，明确记档而不是假装它不存在。

## 十三、明确不做

- 不给 `cmd_open` 补测试脚手架（超范围，那是 T-12 的地盘）
- 不动 `q=2`、不发 `a=q` 探针（PRD 非目标）
- 不统一 agent 与 core 两份 Ollama 配置（PRD 风险 3，独立任务）
- 不给 `core/ai` 的视觉推理路径加预检（PRD 非目标：`core` 禁止主线程阻塞 IO）
- notice 通道不做优先级、不做持久化、不做滚动，只是一个一次性队列
