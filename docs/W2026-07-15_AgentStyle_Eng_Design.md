# 目标三 Eng Design：Agent 支持 Apply Recipe（LLM 自动选风格）

## 一、背景

`docs/W2026-07-15_PRD.md`"目标三"：`Curate` 选完图之后，新增一个 `Style` Stage，用 LLM 看每张选中图的内容，从目标二产出的 9 个预设里挑一个合适的风格，调用 `set_image_recipe` 应用。范围明确是"LLM 自动选，不是用户点名"，复用现有"结构化配置增量+受影响 Stage 集合"调整模型（`docs/M4_Agent_Workflow_Design.md`），闸门默认打开、具体档位留 Eng Design 定。

**对 PRD 原文的修正**：PRD"目标三"一节把"用 LLM 看图内容...选一个合适的风格"写在 `agent/stages/style.py`（Python）里。现有代码严格遵守一条边界："照片分析 AI 归 core（C++），语言/创作 AI 归 agent（Python）"——`EvaluateStage` 自己不碰 LLM，靠 `pzt eval` 让 C++ 去看图；`agent/compose/llm_client.py` 只有纯文本 LLM 调用，没有图片上传能力。这次讨论后确认：**看图选风格这一步放在 C++ core 侧**，Python 的 `StyleStage` 只做编排（调用两个新的 headless 命令），不直接碰 LLM 或图片像素。PRD 文本以本文档为准。

## 二、`core::ai::style`

新增 `core/ai/style.h`/`style.cpp`，跟 `core/ai/evaluation.h`/`evaluation.cpp` 同构（不塞进 `evaluation.cpp`——那个文件明确限定在"曝光/构图/对焦"三维打分的 schema 上）：

```cpp
namespace pzt::core::ai {

enum class StyleError { MissingApiKey, NetworkError, HttpError, ParseError, Hallucinated };

struct StyleSuggestion {
  std::string recipe_name;
  std::string reasoning;
};

Result<StyleSuggestion, StyleError> request_style_suggestion(
    const decode::DecodedImage& image, const std::vector<std::string>& preset_names,
    Provider provider, const LocalModelConfig& local_config = LocalModelConfig{});

namespace detail {
Result<StyleSuggestion, StyleError> request_style_suggestion_impl(
    const decode::DecodedImage& image, const std::vector<std::string>& preset_names,
    Provider provider, HttpPostFn http_post,
    const LocalModelConfig& local_config = LocalModelConfig{});
}

}  // namespace pzt::core::ai
```

`preset_names`：候选预设名单，由调用方（headless 命令）传入，已经从 `list_presets()` 过滤掉 `Origin`（id=0）——Style 的职责就是"选一个风格"，"不选"不是一个有意义的候选项。`core/ai` 不碰 DB，候选集由调用方决定，跟 `evaluation.h` 的定位一致。

**Prompt 构造**：`build_style_prompt(preset_names)` 把候选名单连同各自的一句话风格摘要拼进 prompt（见下一节），让模型有信号判断"这张照片适合哪个"，而不是干猜 9 个纯地名+年份。`build_style_schema_instruction()` 描述 `{"recipe_name": <string>, "reasoning": <one short sentence>}`。

**Schema 约束**：`build_style_json_schema(preset_names)` 只给 `Provider::Local` 用（`local_json_schema` 参数只有 Local 分支消费，Claude/Gemini 分支不消费——这是 `core/ai/ai.cpp` 既有设计）。`recipe_name` 字段用 JSON Schema 的 `"enum": preset_names` 做结构化约束，比 `evaluation.cpp` 现有的 schema 更进一步——候选集在运行时才知道，不是固定 schema。

**校验（对全部三个 provider 都要做，不能只依赖 Local 的 schema 约束）**：解析出 `recipe_name`/`reasoning` 后：
1. 字段缺失或类型不对 → `ParseError`（照抄 `evaluation.cpp` 对 `exposure`/`composition`/`focus`/`comment` 的检查方式）。
2. `recipe_name` 不在 `preset_names` 里（`std::find`）→ `StyleError::Hallucinated`。这是新错误变体，直接类比 `evaluation.cpp::parse_dimension` 的"分数越界→`OutOfRange`"——都是"模型返回的值解析成功但语义不合法"，同一个"信任但要验证"精神。

`map_request_error(RequestError) -> StyleError` 照抄 `evaluation.cpp` 的四分支模式（`MissingApiKey`/`NetworkError`/`HttpError`/`ParseError`）。

## 三、风格摘要小表

写死在 `style.cpp` 匿名命名空间里，9 个预设名字 → 一句话描述，内容取自 `docs/W2026-07-15_RecipeExpansion_Eng_Design.md` 第五节：

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

查不到的名字（未来加了新预设但没同步这张表）直接退回裸名字，不阻塞。**这是跟 `core/recipe/recipe.cpp::builtin_presets()` 数值表的第二份手写副本，有漂移风险**——以后加/改预设要记得两边一起改。之所以接受这个重复而不是给 `core::recipe::PresetSummary` 加 `description` 字段：`core/recipe` 这周已经定型（目标二刚完成），且描述文本是"给 AI 看的 prompt 内容"，跟"预设本身的数据模型"是不同性质的东西，不值得为此改数据结构。

## 四、headless 命令：`pzt recipe suggest` / `pzt recipe apply`

加进 `cmd_recipe` 现有的动词分发（`cli/commands/commands.cpp`，跟 `list`/`rename`/`delete` 并列）。

**为什么拆成两个命令而不是一个**：PRD 非目标一节明确要求"确认自动选的实现方式不会让手动指定变得更难加（接口层面留个口子即可）"。`apply <recipe_name>` 是一个纯粹的、独立的"按名字应用"操作，本身就是这个口子——未来"用户点名风格"的手动路径直接调 `apply`，不需要经过 `suggest`。

**`pzt recipe apply <project> <image_path> <recipe_name> --json`**（纯 setter，不碰 AI）：
- `resolve_project_json(positional[0])` → `find_image_by_path(*project_id, positional[1])` → `find_preset_by_name(positional[2])`（已有）→ `core::set_image_recipe`。
- 错误码：`image_not_found` / `recipe_not_found` / `set_recipe_failed`。
- 成功：`{"applied": true, "recipe_name": "<name>"}`。

**`pzt recipe suggest <project> <image_path> --provider <gemini|claude|local> --json`**（只读，不改库）：
- `--provider` 解析照抄 `cmd_eval`（必填，不认识的值报 `usage`）。
- 新增一个本地小帮手 `decode_image_for_ai(ImageId)`，复刻 `core/ai/evaluation_worker.cpp` 里 RAW-vs-JPEG 预览路径解析 + `decode_preview_file` 调用——这是一份小的、可接受的重复，这次不为此新增 `core/api` 门面函数。
- 候选列表 = `list_presets()` 过滤掉 `id==0`。
- 调 `core::ai::request_style_suggestion`。
- 错误码：`missing_api_key` / `network_error` / `http_error` / `parse_error` / `hallucinated`（新增 `style_error_str` 映射函数，照抄 `evaluation_error_str` 的模式），加上解码失败的 `image_unavailable`。
- 成功：`{"recipe_name": "<name>", "reasoning": "<text>"}`。

不新增 C++ 单元测试——仓库现有约定是 headless 命令只有 shell 级覆盖（`headless_smoke.sh`），`commands.cpp` 本身没有对应的 C++ 单测文件。

## 五、`agent/stages/style.py::StyleStage`

`@dataclass`，跟 `CurateStage` 同构：`name="Style"`，`inputs=["Curate"]`，`cost_class="cloud"`，`criticality="optional"`。

`run()` 读 `ctx.outputs["Curate"].data["selected"]`（图片路径列表），对每张图：
1. `self.client.call("recipe", "suggest", ctx.project_id, path, "--provider", provider)` 拿到 `recipe_name`。
2. `self.client.call("recipe", "apply", ctx.project_id, path, recipe_name)`。
3. 成功收进 `applied: {path: recipe_name}`；`PztCommandError` 收进 `skipped: [{path, error}]`。

全部失败且 `selected` 非空 → `StageOutput(ok=False, ...)`；否则 `ok=True`（部分失败通过 `skipped` 可见，不拖垮整个 Stage）。`criticality="optional"` 意味着即使 `ok=False`，`Deliver` 依然会继续跑，只是交付未风格化的图，不会卡死整个 Run。

## 六、三个入口的接入

**`plan_composer.py::compose_plan()`**：在 `Curate`/`Deliver` 之间插入 `StageSpec(name="Style", params={"provider": decision.get("provider", "gemini")}, gate="courtesy")`，复用已有的 `decision["provider"]` 字段。`gate_on_timeout` 用 `StageSpec` 默认值 `"proceed"`，正好是"超时自动放行"。

**闸门档位 = `courtesy`**：问一句"已按 LLM 选择自动套用风格，是否需要调整？"，超时不回复就采纳 LLM 的选择继续走 `Deliver`——不阻断自动化流程这个初衷，出错了也就是一张照片的调色不合适，不是灾难性后果。

**三个入口都要接上 Style**（`run_telegram.py`/`run_intent.py`/`run_watchfolder.py` 各自的 `stages` dict 加 `"Style": StyleStage(client=client)`），但只有 `run_telegram.py`（走 `agent/router/session_router.py`）真正处理 `AWAITING_GATE` 状态、能问用户问题。`run_intent.py`/`run_watchfolder.py` 是无人值守的脚本化入口，这次给它们的主循环加一段：遇到 `AWAITING_GATE` 就立刻调 `Driver.timeout_gate(run)`（等价于闸门瞬间超时，直接采纳 LLM 的选择），不新写交互逻辑：

```python
while run.status in (RunStatus.RUNNING, RunStatus.AWAITING_GATE):
    if run.status == RunStatus.AWAITING_GATE:
        driver.timeout_gate(run)
        print(f"  Style 闸门自动放行(非交互入口) [{run.status.value}]")
        continue
    driver.advance(run)
    print(...)
```

（`run_intent.py` 有两处这样的循环——初始一次 + `AWAITING_REVIEW` 调整之后重跑一次——两处都要改；`run_watchfolder.py` 只有一处。）

## 七、`Deliver` 去重标记的修复

`agent/stages/deliver.py::DeliverStage.inputs` 从 `["Curate"]` 改成 `["Style"]`——`Driver._downstream_of` 按 `inputs` 做 BFS 判断"调整某个 Stage 之后哪些下游要重跑"，不改的话单独调整 Style 不会触发 `Deliver` 重新进 `PENDING`，新风格永远发不出去。

改了 `inputs` 之后还不够：`_marker_path` 现在只 hash 选中图片的路径列表，不看套了哪个风格。一次 Run 交付过之后单独调整 Style（换风格，图片路径不变），标记会误判成"已经交付过"，`Deliver` 重跑了但被自己的去重逻辑短路，新风格依然没有真正到达交付文件。修法：

```python
def _marker_path(self, run_id: str, selected: List[str], applied_styles: Dict[str, str]) -> Path:
    style_sig = "|".join(f"{p}={applied_styles.get(p, '')}" for p in selected)
    digest = hashlib.sha256((("|".join(selected)) + "||" + style_sig).encode("utf-8")).hexdigest()[:16]
    return Path(self.marker_dir) / f"{run_id}-{digest}.json"
```

`run()` 里从 `ctx.outputs.get("Style")` 取 `applied_styles`（`Style` 没跑过/被跳过时 `{}`），摘要退化成只看路径列表，行为不变——不破坏"Style 之前就存在"的旧场景。

## 八、任务分解

1. 本文档。
2. `core::ai::style`（看图选风格 + 校验）。
3. headless 命令 `pzt recipe suggest`/`pzt recipe apply`。
4. `agent/stages/style.py`。
5. 接入三个入口 + `Deliver.inputs`。
6. `Deliver` 去重标记编入套用的风格。
7. 真机验证。

具体步骤见 `.claude/plans/fully-understand-the-pzt-iterative-gosling.md`（本次实现用的 TDD 计划）。
