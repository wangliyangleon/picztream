# 目标三 Eng Design：Agent 支持 Apply Recipe（LLM 选风格）

> **已归档(2026-07-19)**：目标三「agent 选风格」已完成，最终形态是本文档第二节起描述的"用户一句话描述 + LLM 纯文本匹配预设 + 整批统一"两段式闸门流程（相对 PRD 原始"看图自动选"的翻转，见第一节设计演变）。**注意跨文档取代**：第七节「`session_router.py` 接入两段闸门」所述的路由层已被目标五的 `agent/session/consumer.py` 取代（单线程 router 已删），两段闸门的行为逐条保留、迁移记录见 `docs/history/W2026-07-15_AgentRuntime_Eng_Design.md` 第八节对齐清单。本周开发目标全貌见 `docs/W2026-07-15_PRD.md`。

## 一、背景与设计演变

`docs/W2026-07-15_PRD.md` 目标三：`Curate` 选完图之后新增 `Style` 能力，用 LLM 从目标二产出的 9 个预设里选一个风格应用；范围是"LLM 自动选，不是用户点名"，闸门默认打开，复用现有"结构化配置增量 + 受影响 Stage 集合"的调整模型。

这个目标在本周经历了一次设计翻转，最终形态跟 PRD 原始设想不同，这一节记录演变，后面各节描述**最终落地的设计**。

**第一版（已实现、随后被取代）**：LLM 逐张看图选风格。因为"照片分析 AI 归 core（C++），语言 AI 归 agent（Python）"这条边界，看图选风格放在 C++ core 侧，新增 `core::ai::style` 模块和 `pzt recipe suggest`/`pzt recipe apply` 两个 headless 命令，Python 的 `StyleStage` 只做编排、逐张调这两个命令。`Style` 不带自己的闸门，跑完后由 `Deliver` 现成的必选闸门顺带展示"每张选了什么风格"给用户复核。这一版完整落地过，代码仍保留在仓库里（见第十节），只是不再在默认流程上。

**翻转的动因（真机验证后）**：逐张 vision 有两个实际问题。一是慢，N 张选中图就是 N 次 vision LLM 调用加 N 次 apply；二是每张各自选风格，结果风格不统一，而用户的真实诉求大多数时候是"这一批所有图风格统一"。

**第二版（最终形态）**：让用户用一句话描述想要的风格，LLM 只用这句话（不看图）从 9 个预设里匹配一个，套到一张代表图上给用户看，确认 OK 之后把同一个风格套到整批选中图再交付。关键性质的变化：

- **从视觉推理变成纯语言推理**。按文字描述匹配预设不需要图像输入，按"视觉归 core、语言归 agent"的同一条边界，这一步落在 agent（Python）侧的 `agent/compose/style_matcher.py`，不再走 core 的 vision 路径。这同时解决了"慢"（一次文本匹配 vs N 次 vision）和"不统一"（一个风格套全部 vs 逐张各选）。
- **从"事后复核逐张结果"变成"两段式对话"**。风格描述、单张预览确认这两次交互，用两个各自带闸门的 Stage 承载（见第二节），而不是塞进 `Deliver` 的闸门。

**PRD 文本以本文档为准**：PRD 目标三写的是"LLM 看图内容自动选"，最终实现是"用户文字描述、LLM 文本匹配、整批统一"。"自动选风格"这个方向本身（对话调整→重跑子图、不重跑 Evaluate/Dedup/Curate）保留，改的是"依据什么选"和"选几次"。

## 二、最终形态：两段式对话流程

`Curate` 之后、`Deliver` 之前插入两个 Stage，各自带一个必选闸门：

1. **`Style`（闸门问描述）**：闸门在 `Style` 运行前触发，问用户"想要什么风格？"。用户回一句描述，`Style` 用这句描述文本匹配出一个预设，套到第一张代表图上，输出 `{chosen_recipe, preview_photo}`。
2. **`StyleApplyAll`（闸门做确认）**：闸门在 `StyleApplyAll` 运行前触发，此时 `Style` 已经跑完，闸门把代表图的套用效果发给用户预览、问"这个风格 OK 吗"。用户确认后 `StyleApplyAll` 才运行，把同一个风格套到其余全部选中图。

**为什么必须拆成两个 Stage**：这是 `Driver.advance()` 闸门语义决定的。闸门在对应 Stage **运行之前**触发，展示的只能是**上游已经算完**的状态，不是这个 Stage 自己还没产生的输出。所以：`Style` 的闸门在它运行前问"想要什么风格"（此时还没有风格，正好问描述）；`StyleApplyAll` 的闸门在它运行前展示 `Style` 已经套好的代表图（此时代表图已就绪，正好做预览确认）。两次交互时机不同、依赖的上游状态不同，落在同一个 Stage 上没法同时满足，必须拆开。

**"不满意"的处理是一个重描述循环**：用户在 `StyleApplyAll` 的预览确认闸门说"不满意/再暖一点"，这句话本身被当成新的风格描述，重跑 `Style`（用新描述重新匹配、重新套代表图），再停回 `StyleApplyAll` 的预览闸门。不是"先说不满意、再单独问一次描述"的两轮制，用户的否定回复直接就是新的描述输入。

## 三、`agent/compose/style_matcher.py`（纯文本匹配）

新增模块，不走 core 的 vision 路径：

```python
class StyleMatchError(Exception):
    def __init__(self, code: str, message: str) -> None: ...

def match_style_description(description, http_post=None, meta_provider="local") -> str: ...
```

把候选预设的名字 + 一句话描述一起拼进 prompt，让 LLM 只用用户的文字描述挑一个，返回 `recipe_name`。**校验**：返回值必须在候选名单里，否则抛 `StyleMatchError("hallucinated", ...)`，跟第一版 `core::ai::style` 的"信任但要验证"是同一个精神，schema 约束（Local provider 可用 enum）是锦上添花、不替代解析后的成员校验。底层 `LlmRequestError`（畸形响应等）原样上抛，不被 `StyleMatchError` 吞掉。

候选表 `_PRESET_DESCRIPTIONS`（9 条，名字→一句话描述）：

| 预设 | 摘要 |
|---|---|
| Havana 1959 | 暖调、高饱和、古巴风情 |
| Tokyo 1966 | 昭和时代暖调、低饱和、柔亮怀旧 |
| Paris 1974 | 暖调、中低饱和、低对比 |
| Miami 1986 | 中性白平衡、高饱和高对比、glossy 商业感 |
| New York 1994 | 冷调、低饱和、高对比、粗颗粒 |
| Shanghai 2010 | 中冷调、高饱和、glossy 商业感、极细颗粒 |
| Munich 1951 | 黑白，极高对比、深邃影调、粗颗粒 |
| Rome 1960 | 黑白，中低对比、柔亮影调 |
| Berlin 1989 | 黑白，高对比、偏暗、情绪紧迫 |

这是这张风格摘要表的**第三份手写副本**（`core/recipe/recipe.cpp::builtin_presets()` 数值表、`core/ai/style.cpp` 的第一份文字副本之后）。`agent/` 按架构约束（`agent -> cli -> core` 单向依赖）不能直接读 C++ 那份表，只能再抄一份，跟 `core/ai/style.cpp` 已经接受的漂移风险是同一个取舍，不是这次新引入的问题。以后加/改预设三处一起改。

## 四、`agent/stages/style.py::StyleStage`（重写）

`@dataclass`：`name="Style"`，`inputs=["Curate"]`，`cost_class="cloud"`，`criticality="critical"`，新增字段 `http_post: Optional[HttpPostFn] = None`。

`run(ctx, params)`：
1. 读 `params["style_description"]`（strip 后为空 → `ok=False`，缺描述没法选）。
2. 读 `ctx.outputs["Curate"].data["selected"]`（为空 → `ok=True, data={"chosen_recipe": None, "preview_photo": None}`，没有图不算失败）。
3. `match_style_description(description, http_post=self.http_post, meta_provider=params.get("provider","local"))` 拿到一个预设名（`StyleMatchError`/`LlmRequestError` → `ok=False`）。
4. `self.client.call("recipe", "apply", project_id, selected[0], recipe_name)` 套到代表图（`PztCommandError` → `ok=False`）。
5. 成功 → `ok=True, data={"chosen_recipe": recipe_name, "preview_photo": selected[0]}`。

**`criticality` 从第一版的 `optional` 改成 `critical`**：现在只有一次风格决策，失败了下游 `StyleApplyAll`/`Deliver` 都没有意义，不再是"逐张、部分失败可跳过"的语义。

**`http_post` 注入点**：`match_style_description` 直接调 `compose/llm_client.py::request_json(..., http_post=...)`，不经过 `self.client` 那条已经被 `PztClient` fake runner 接管的子进程边界。测试必须能把 `http_post` 换成假实现，否则 `router_fakes.py` 跑到 `Style` 真正执行时会发真网络请求到本地 Ollama，重演本会话早些时候"provider 默认 local 后 pytest 挂起"的事故（同一类坑，见第十一节）。

## 五、`agent/stages/style_apply_all.py::StyleApplyAllStage`

`@dataclass`：`name="StyleApplyAll"`，`inputs=["Style"]`，`cost_class="local"`，`criticality="optional"`（个别照片套用失败不该拖垮整批交付）。

`run(ctx, params)`：从 `ctx.outputs["Style"]` 取 `chosen_recipe`/`preview_photo`，从 `ctx.outputs["Curate"]` 取 `selected`。
- `chosen_recipe` 为空 → `ok=True, data={"applied": {}}`（`Style` 没选出风格，没什么好套，跟"没有选中照片"同类"无操作"语义）。
- 否则 `preview_photo` 直接记进 `applied`（`Style` 里已经套过，不重复调用），其余 `selected` 逐张 `recipe apply`，失败进 `skipped`。
- 其余照片全部失败（`remaining` 非空且全进 `skipped`）→ `ok=False`；否则 `ok=True`。

`inputs=["Style"]` 只用于依赖排序；`run()` 直接读 `ctx.outputs["Curate"]` 是安全的，因为 `Driver._run_stage` 传给 `stage.run()` 的 `ctx.outputs` 是完整的 `run.outputs`，不是按 `inputs` 过滤过的子集（`DeliverStage` 也在用这个特性）。

## 六、`Driver.rerun_stage`（新增编排原语）

第二节的"重描述循环"需要一个能力：闸门已经问过、用户这次回复就是答案，要用新 params 重跑 `Style`，但**不能**再触发一次 `Style` 自己的闸门（否则又问一遍"想要什么风格"）。现有的 `apply_adjustment` 做不到：它重置目标 Stage 和下游为 `PENDING`、清 `gate_state`，但下一次 `advance()` 发现 `Style` 又是 `PENDING` 且 `gate="required"`，会重新触发闸门。

新增 `Driver.rerun_stage(run, stage_name, params)`：更新 `spec.params`，把下游重置成 `PENDING` 并清空对应 `outputs`，清 `gate_state`，`status=RUNNING`，然后**直接调 `_run_stage`**（绕过 `advance()` 的闸门检查）跑这个 Stage。跟 `resolve_gate` 一样，跑完之后是否继续推进到下一个闸门/`AWAITING_REVIEW` 由调用方（router 的 `_drive_to_stop_and_notify`）负责，`rerun_stage` 本身只保证目标 Stage 被用新 params 直接跑一遍。

## 七、`session_router.py` 接入两段闸门

**`_STAGE_PROGRESS_MESSAGES` 去掉 `"Style"`**（也不为 `StyleApplyAll` 新增）。这个表是 RUNNING 期间每个 Stage 运行前发的通用进度提示。`Style`/`StyleApplyAll` 现在都带必选闸门，`_drive_to_stop` 会在它们运行前先发一句通用进度、紧接着 `advance()` 就因为闸门暂停，对 `Style` 来说"正在自动套用风格..."会紧贴在闸门自己的"想要什么风格？"前面自相矛盾。它们各自闸门的消息（"想要什么风格？"/预览确认）本身就是恰当的提示。`Deliver` 的进度消息保留不动。

**`_handle_gate` 按闸门 Stage 分流**：在通用关键词判断之前，`run.gate_state.stage_name in ("Style","StyleApplyAll")` 时转 `_handle_style_gate`。新增的 `_handle_style_gate(run, stage_name, text, normalized)`：
- 命中 `_REJECT_KEYWORDS` → `cancel` + "已取消"（跟通用逻辑一致）。
- `stage_name == "StyleApplyAll"` 且命中 `_APPROVE_KEYWORDS` → `resolve_gate(run, "proceed")` + `_drive_to_stop_and_notify`（确认这个风格，套用全部、推进到 `Deliver` 闸门）。
- `stage_name == "Style"` 且文本为空 → 提示"跟我说说想要什么风格吧"，不把空回车当空描述丢给 LLM。
- 其余（`Style` 收到任意非空文本 / `StyleApplyAll` 收到非同意非取消文本）→ 这句话就是（新的）风格描述 → 发"正在选风格.../正在重新选风格..." → `driver.rerun_stage(run, "Style", {"style_description": text})` → `_drive_to_stop_and_notify`。

这些闸门回复**不走** `classify_gate_reply_fn` 那套"同意/拒绝/结构化调整"分类：两个闸门问的都是开放式问题，用户的自由文本本身就是答案，不该再套一层 LLM 分类。

**`_drive_to_stop_and_notify` 的 `AWAITING_GATE` 分支按闸门 Stage 分流**：
- `"Style"` → 发文本"想要什么风格？用一句话描述就行，比如'复古暖色调'"。
- `"StyleApplyAll"` → `_send_style_preview(run)`：导出并发送 `Style` 输出里的 `preview_photo`，配文"这是用「{chosen_recipe}」套用的效果，OK 就回复'好的'，不满意直接说想要什么风格，不要了就说'取消'"；导出/发送失败的降级照抄 `_send_preview` 的 `send_photo` → `send_file` → 提示 三级降级。
- 其它（`Deliver`）→ 现状 `_send_preview(run)`。`Deliver` 闸门的选片小结改成从 `run.outputs["StyleApplyAll"]` 读 `applied`、显示"已套用风格「{chosen_recipe}」"。

## 八、`Deliver` 接 `StyleApplyAll` + 去重标记

- `DeliverStage.inputs` 默认值改回 `["Curate"]`（撤销第一版临时改成的 `["Style"]`）。因为现在只有 `run_telegram.py` 的 Plan 含 `Style`/`StyleApplyAll`，另两个入口完全不含，默认值不能假设它们存在。`run_telegram.py`（及 `router_fakes.py`）构造 `DeliverStage` 时显式传 `inputs=["StyleApplyAll"]` 覆盖默认值，这样"调整风格触发 `Deliver` 重跑"这条 `_downstream_of` BFS 依赖对 `StyleApplyAll` 生效。
- `run()` 从 `ctx.outputs.get("StyleApplyAll")` 取 `applied_styles`（覆盖全部照片的完整映射来自 `StyleApplyAll`，`Style` 只有代表图那一张）。另两个入口的 Plan 没这个 Stage，取到 `None` 自然退化成 `{}`，跟"完全没风格化"一致，不需要额外分支。
- `_marker_path` 的哈希逻辑不变（已经是从 `applied_styles` dict 算风格签名 + 选片路径列表），来源换了但形状没变：同一批选片配不同风格必须被当成"不同的一次交付"，不被去重标记误判成已交付过。

## 九、入口范围：只接 `run_telegram.py`

新 Style 流程天然需要对话（问描述、确认预览），只接用户实际使用的 `run_telegram.py`：

- **`run_telegram.py`**：`stages` dict 加 `"StyleApplyAll"`，`DeliverStage(..., inputs=["StyleApplyAll"])`。`StyleStage` 用真实网络请求（不注入 `http_post`）。
- **`run_watchfolder.py`**：本来就设计成"不含对话式交互、全自动跑到底"，跟需要对话的 Style 直接冲突。`build_plan` 去掉 `Style`，回到 `Ingest->Evaluate->Dedup->Curate->Deliver` 五段，`DeliverStage` 用回默认 `inputs=["Curate"]`。
- **`run_intent.py`**：有 `input()` 交互循环，但目前完全没有处理 `AWAITING_GATE` 的代码，硬塞两段必选闸门工作量明显更大，本次不做。它的 Plan 来自跟 `run_telegram.py` 共用的 `compose_plan()`（会带上 `Style`/`StyleApplyAll`），在 `validate_plan(compose_plan(...))` 之后加一行 `plan.stages = [s for s in plan.stages if s.name not in ("Style","StyleApplyAll")]` 过滤掉，不改 `compose_plan()` 本身。

`plan_composer.py` 在 `Curate`/`Deliver` 之间插入 `StageSpec(name="Style", params={"provider":...}, gate="required")` 和 `StageSpec(name="StyleApplyAll", gate="required")`；`validate.py` 的 `_EXPECTED_STAGE_NAMES` 加 `"StyleApplyAll"`（`bad_style_provider` 校验仍只针对 `Style`，`StyleApplyAll` 不调 LLM、无 provider）。`Style.params["provider"]` 的含义从"vision provider"变成"文本匹配的 meta-provider"，仍复用 `local`/`gemini`/`claude` 同一套词汇表，校验规则不变。

## 十、保留但不再默认使用：`core::ai::style` + `pzt recipe suggest`（vision 路径）

第一版的 vision 逐张选风格路径**代码仍在仓库里**，只是不再被任何默认流程调用，作为一个可用能力和"用户点名风格"手动路径的口子保留，不删除：

- `core/ai/style.h`/`style.cpp`：`request_style_suggestion(image, preset_names, provider, ...)`，跟 `core/ai/evaluation.*` 同构，含 `StyleError::Hallucinated` 校验、`Provider::Local` 的 `enum` schema 约束、9 条风格摘要表（第一份文字副本）。
- `pzt recipe apply <project> <image> <recipe_name> --json`：纯 setter（`set_image_recipe` 薄壳），**这个命令新流程仍在用**（`StyleStage`/`StyleApplyAllStage` 套用风格靠它）。错误码 `image_not_found`/`recipe_not_found`/`set_recipe_failed`。
- `pzt recipe suggest <project> <image> --provider ... --json`：看图选风格（只读）。这条命令是 vision 路径专属，新流程不再调用，但保留着：未来若要做"看图自动选"或"用户点名"的增强，`apply` 是现成的应用口子、`suggest` 是现成的看图口子。

不删除的理由：它们是完整、正确、有测试覆盖的能力，只是不在当前默认对话流程上；删掉等于丢掉一条已经验证过的 vision 能力和手动路径的地基。

## 十一、Provider 默认值改成 local（quota 考虑）

跟 Style 本身不是同一件事，但在本周接线过程中一起做了：`Evaluate` 的视觉 provider 默认值、以及 `compose_plan`/`parse_adjustment`/`match_style_description` 等纯文本"意图/调整/风格匹配"用的 `meta_provider` 默认值，统一从 `"gemini"` 改成 `"local"`（Ollama），避免每次开发/测试都消耗云端 API 额度。两条调用链路独立：视觉走 `core::ai::request_json`（`Provider::Local` 已在目标一落地），文本走 Python 侧 `agent/compose/llm_client.py`（本周新增 Ollama 分支，不需要 API key，POST 到 `http://localhost:11434/api/chat`，响应形状 `message.content`，是 `core/ai/ai.cpp` 对应逻辑的纯文本镜像）。

**副作用与修法**：`router/session_router.py` 里 `classify_gate_reply_fn`/`classify_collecting_message_fn`/`refine_plan_confirmation_fn` 以及新流程的 `StyleStage.http_post` 这些调用点都没有强制注入 `http_post`。改默认 `local` 之前，测试隐式依赖"provider=gemini 且没设 key 会立刻报 `missing_api_key`"来避免真发网络请求；改成 `local` 后没有 key 检查，会直接尝试连真实的 `localhost:11434`，在沙盒里既不成功也不快速失败，导致 pytest 挂起。修法：给测试 fixture 里所有会走文本 LLM 的调用点都注入立即返回/立即失败的假实现，`router_fakes.py` 的 `classify_collecting_message_fn` 默认抛 `AdjustmentError`（复现"分类失败就照老办法"的降级路径），`StyleStage` 注入 `_fake_style_http_post`（返回 Ollama 形状的固定响应）。

**生产权衡**：真实 bot 场景下，用户在闸门回复的非关键词文本会走本地 Ollama，如果 Ollama 没启动或很慢，这次回复要等到超时才降级，不再是过去因没配云端 key 而立刻报错。这是"默认走本地、省额度"这个选择自带的权衡，不是新缺陷。

## 附：任务分解与验证

实现按 TDD 分解为：`style_matcher` → `Driver.rerun_stage` → 重写 `StyleStage` → 新增 `StyleApplyAllStage` → `Deliver` 接 `StyleApplyAll` → `plan_composer`/`validate` 接入 → `session_router` 两段闸门 → 三入口收口 → 全量验证，每步先 RED 后 GREEN、一步一 commit。真机验证：`run_telegram.py`（需本地 Ollama）发图→意图确认→`Curate` 后收到"想要什么风格？"→回一句描述→收到单张预览+"这是用「X」套用的效果，OK 吗"→回一句非同意的话确认真的重挑一次→"好的"→收到"选好了 N 张"的 Deliver 预览→交付。
