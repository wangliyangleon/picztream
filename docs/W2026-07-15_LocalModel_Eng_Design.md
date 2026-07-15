# PicZTream (PZT) 目标一工程设计：本地模型支持（Provider::Local / Ollama）

## 背景

产品需求见 `docs/W2026-07-15_PRD.md` 目标一：云端评估（Claude/Gemini）依赖用户自己的 API key 额度，本地开发/测试常被限流卡住，需要一条不依赖外部配额的本地评估路径。本文档只回答"具体怎么落地"，不重复 PRD 已经拍板的范围/非目标/验收标准。

## 一、现有代码基础（全部可直接复用/扩展，不推倒重来）

- **`core::ai::request_json`**（`core/ai/ai.h:56-60`，实现在 `ai.cpp:292-358`）：唯一的"发一次带图片的 AI 请求、按调用方描述的形状要 JSON"入口，当前只认 `Provider::Claude`/`Provider::Gemini` 两路分支。`HttpPostFn` 注入缝已经存在（`ai.h:44-46`），单测不需要真的连网络。
- **`core::ai::request_evaluation`**（`evaluation.cpp:160-164`，内部 `detail::request_evaluation_impl`）：完全 provider 无感知——只是拼好 `user_prompt`/`schema_instruction` 调 `request_json`，再用同一套 `parse_dimension`/`parse_exposure_fix`/`parse_composition_fix` 解析结果。**这意味着 `Provider::Local` 一旦让 `request_json` 吐出跟 Claude/Gemini 同形状的 JSON，评估解析逻辑一行都不用改**——PRD 里"对现有异步评估队列/DB/去重逻辑零改动"的说法在这里被验证成立。
- **`core::ai::EvaluationWorker`**（`evaluation_worker.h`）：后台 `jthread` 队列，`EvaluationFn` 类型别名（`h:27-28`）是测试注入假函数的口子；`PZT_FAKE_EVAL` 环境变量（`commands.cpp:349-372`）是给 agent watch-folder 端到端测试用的"完全跳过真 AI 调用"逃生舱——这解决的是不同的问题（测试时不想花钱/被限流），跟 `Provider::Local`（测试时想要真实但免费的评估）是互补关系，不是重叠或替代。
- **`core::settings::Settings`**（`settings.h:17-49`）：已经是"可调行为参数放这里、调用方读一次显式传参"的既定模式（`curate_time_window_seconds` 独立于 `dedup_time_window_seconds` 就是这个模式的先例，见 `docs/M4_Eng_Design.md` 第三节）。
- **`cli/commands/commands.cpp:cmd_eval`**（`291-380` 附近）与 **`cli/commands/browse.cpp:resolve_ai_provider`**（`57-67`）：两处现有的 provider 字符串解析入口，分别对应 headless 命令和交互式 TUI。

## 二、Ollama API 调研结论（WebFetch 核实，`/api/chat`）

- 图片：`message.images` 字段，纯 base64 字符串数组（**没有** data URI 前缀，跟 Claude 的 `source.data`/Gemini 的 `inline_data.data` 都不一样，需要单独的请求体构造函数）。
- 结构化输出：`format` 字段支持 `"json"`（宽松模式，只保证语法合法）或者一个完整 JSON Schema 对象（强约束）。**本设计选宽松 `"json"` 模式**，理由见上方"已确认的关键设计决策"第 1 条——保留 `request_json` 的任务无感知设计，只吃"保证合法 JSON"这一个免费收益，不吃"强约束字段形状"这个需要打破架构边界才能拿到的收益。完整 Schema 约束列进"风险与待确认问题"当未来可选增强。
- 响应：`message.content` 是纯文本（跟 Claude 的 `content[0].text`、Gemini 的 `candidates[0].content.parts[0].text` 同构，现有的 `parse_inner_json`/`strip_markdown_json_fence` 可以直接复用）。
- 认证：无需 API key/认证头，默认监听 `http://localhost:11434`。
- 超时/模型加载：文档没有明确的"模型是否已加载"探测接口，`keep_alive` 参数控制卸载前的保活时长（默认 5 分钟）。冷启动（模型第一次被请求，需要真正载入内存）耗时不可忽视，尤其在 16GB 内存的机器上——这是选超时数值时要考虑的因素，见下方"超时"一节。

## 三、`core/ai` 侧改动

### `Provider` 枚举与 `to_string`（`ai.h:20`、`ai.cpp:13-15`）

```cpp
enum class Provider { Claude, Gemini, Local };
```
`to_string` 从三元表达式改成小 switch，新增 `"local"` 分支——落库（`image_evaluations.provider`）和 debug 日志复用同一份映射，跟现有约定一致。

### 新增 `LocalModelConfig`（`ai.h`，`Provider` 枚举下方）

```cpp
struct LocalModelConfig {
  std::string base_url = "http://localhost:11434";
  std::string model = "moondream";
};
```
纯数据，可默认构造——测试和"调用方偷懒不传"的场景都能编译通过，不强制每个调用点都显式构造。

### `get_api_key` 改造（`ai.cpp:26-31`）

`Provider::Local` 不需要 key，直接跳过这一路。`request_json` 顶部的检查改成：
```cpp
if (provider != Provider::Local) {
  auto api_key = get_api_key(provider);
  if (!api_key) return Result<...>::Err(RequestError::MissingApiKey);
}
```
（`api_key` 的后续使用点——header 构造——本来就已经在 `if (provider == Provider::Claude)`/`else`（Gemini）分支里，改成三路 `if/else if/else` 之后 `Local` 分支天然不碰 `*api_key`。）

### `request_json` 签名扩展（`ai.h:56-60`）

新增一个**默认构造**的尾参数，Claude/Gemini 路径完全无感知（不强制任何现有调用点改动）：
```cpp
Result<nlohmann::json, RequestError> request_json(const decode::DecodedImage& image,
                                                    const std::string& user_prompt,
                                                    const std::string& schema_instruction,
                                                    Provider provider,
                                                    HttpPostFn http_post = perform_curl_post,
                                                    const LocalModelConfig& local_config = LocalModelConfig{});
```
内部三路分支（`ai.cpp:312-324` 现有的 if/else 改成 if/else if/else）：
```cpp
} else if (provider == Provider::Local) {
  url = local_config.base_url + "/api/chat";
  headers = {{"content-type", "application/json"}};
  request_body = build_local_request(image_base64, instruction_text, local_config.model);
}
```

### 新增 `build_local_request`/`parse_local_response`（`ai.cpp` 匿名命名空间，跟 `build_claude_request`/`parse_claude_response` 平级）

```cpp
nlohmann::json build_local_request(const std::string& image_base64,
                                    const std::string& instruction_text,
                                    const std::string& model) {
  return {
      {"model", model},
      {"format", "json"},
      {"stream", false},
      {"messages", nlohmann::json::array({
          {{"role", "user"}, {"content", instruction_text}, {"images", nlohmann::json::array({image_base64})}}
      })},
  };
}

Result<nlohmann::json, RequestError> parse_local_response(const std::string& body) {
  nlohmann::json outer;
  try { outer = nlohmann::json::parse(body); }
  catch (const nlohmann::json::parse_error&) { return Result<...>::Err(RequestError::ParseError); }
  if (!outer.contains("message") || !outer["message"].contains("content") ||
      !outer["message"]["content"].is_string()) {
    return Result<...>::Err(RequestError::ParseError);
  }
  return parse_inner_json(outer["message"]["content"].get<std::string>());
}
```
`stream: false` 是必须的——默认流式返回会拆成多个 JSON 对象，现有 `perform_curl_post`/`write_callback` 是"整个 body 攒成一个字符串"的一次性读取模型，不支持逐行解析 SSE 风格的流式响应，显式关流保持跟 Claude/Gemini 一样的"一次请求、一个完整 JSON 响应"模型。

### 超时（留给任务 5 决定，本增量任务 2 不做）

`perform_curl_post` 目前是 `HttpPostFn`（`request_json` 的 `http_post` 参数类型，固定三参数：`url, headers, body`）签名的默认实现——`HttpPostFn http_post = perform_curl_post` 这个默认参数依赖 `perform_curl_post` 的函数类型精确匹配三参数才能转换成 `std::function`。给 `perform_curl_post` 加一个（哪怕带默认值的）第四个 `timeout_seconds` 参数会让它的类型变成四元，不再能当 `HttpPostFn` 的默认值——编译不过。

真要按 provider 差异化超时，唯一干净的路径是把 `HttpPostFn` 本身的签名改成四参数，这会牵连 `ai_test.cpp` 里已有的所有 `fake_post` 假函数（不止 Local 相关的新增用例，Claude/Gemini 原有用例的假函数也要跟着改），量级远超"加一个参数"。这个能力目前也用不上——`fake_post` 测试从不真的等待超时，唯一需要更长超时的场景是任务 5 真机连真实 Ollama。**决定：超时参数化整体挪到任务 5**，跟真机测出来的冷启动/推理耗时数据一起决定要不要区分"冷启动首次请求"和"模型已加载"两种超时、以及是否值得为此改 `HttpPostFn` 签名。任务 2-4 期间，`Provider::Local` 走跟 Claude/Gemini 完全相同的硬编码 60 秒总超时/10 秒连接超时。

## 四、`core::ai::EvaluationWorker`/`request_evaluation` 侧的涟漪

**这是本设计范围最大的一处改动**，因为 `LocalModelConfig` 要从 `cmd_eval`/`browse.cpp` 一路显式传到 `request_json`，中间经过三层：

1. `EvaluationFn` 类型别名（`evaluation_worker.h:27-28`）：从 3 参数扩到 4 参数
   ```cpp
   using EvaluationFn = std::function<Result<EvaluationResult, EvaluationError>(
       const decode::DecodedImage&, const std::string&, Provider, const LocalModelConfig&)>;
   ```
   **这个类型是 `std::function`，不是普通函数——不能"默认参数"绕过去，每一个赋值给它的 lambda 都要显式接收第四个参数**（哪怕是 provider 不是 Local 时忽略）。受影响的现有 lambda：`request_evaluation`（真实实现）、`commands.cpp:349-361` 的 `PZT_FAKE_EVAL` 假函数、以及 `evaluation_worker_test.cpp` 里任何现有的假 `EvaluationFn`（实施时要逐个找出来改签名，这是任务分解里单独一步，不能漏）。
2. `EvaluationWorker::request()`（`evaluation_worker.h:49-50`）与内部 `PendingRequest`（`h:90-95`）：新增 `LocalModelConfig` 字段/参数，随请求一起入队——这跟 `auto_reject` 已经是"随请求显式传入、不读全局状态"的先例（P6 物理隔离）完全同构，不是新模式。
3. `request_evaluation`/`detail::request_evaluation_impl`（`evaluation.cpp:121-124,160-164`）：签名各加一个 `const LocalModelConfig&` 参数，原样透传给 `request_json`。

**顶层调用点**：`cmd_eval`（`commands.cpp`）在解析完 `--provider` 之后，若为 `local`，读一次 `core::load_settings()` 取 `ollama_base_url`/`ollama_model` 构造一个 `LocalModelConfig`（非 local 时传默认值，反正不会被用到），传进 `worker.request(...)`。`browse.cpp` 的交互路径同理，在 `resolve_ai_provider()` 旁边加一个 `resolve_local_model_config()`（或者直接内联，看实施时哪个更干净）。

## 五、CLI 接线

- `cmd_eval`（`commands.cpp:313-320`）：`provider_str` 解析加 `else if (provider_str == "local") provider = Provider::Local;`，usage 提示字符串同步改成 `--provider <gemini|claude|local>`。
- `resolve_ai_provider()`（`browse.cpp:57-67`）：`PZT_AI_PROVIDER` 环境变量解析加 `if (value == "local") return Provider::Local;`。`Settings.ai_provider` 本来就是 `ai::Provider` 类型，`local` 作为持久化默认值天然可行，不需要额外改 `Settings` 结构本身（`ollama_base_url`/`ollama_model` 是两个新增的独立字段，见下节）。

## 六、`Settings` 扩展（`core/settings/settings.h`/`settings.cpp`）

```cpp
std::string ollama_base_url = "http://localhost:11434";
std::string ollama_model = "moondream";
```
落盘/解析走 `settings.cpp` 现有的"逐字段独立容错"模式（`assign_if_present`，参照 `auto_ai_reject` 那一行的写法），不需要新机制。

## 七、测试策略

- **`core/tests/ai_test.cpp`**：对称新增跟现有 Claude/Gemini 用例平级的 `TEST_CASE`（复用同一个 `fake_post` 注入模式，不连真实网络）：
  - `"request_json parses an Ollama-shaped response"`
  - `"request_json builds a local request with images array (no data URI prefix) and format=json"`
  - `"request_json reports ParseError when Ollama response shape is unexpected"`
  - `"request_json skips the API key check for Provider::Local"`（验证不传任何 key 环境变量也不返回 `MissingApiKey`）
  - `downscale_for_upload`/`strip_markdown_json_fence` 相关用例不需要为 Local 重复——那两个函数不分 provider。
- **`core/tests/evaluation_worker_test.cpp`**：找出所有现有 `EvaluationFn` 假函数，改签名接四个参数；新增至少一条覆盖"`LocalModelConfig` 随请求一起入队、原样传到 `evaluation_fn_`"的用例。
- **`cli/tests/headless_smoke.sh`**：`pzt eval --provider local` 的 usage 分支冒烟（不要求真的连 Ollama——跟现有 `--provider bogus` 失败用例同级别，只加一条"local 是合法值，走到网络调用那一步"的最小验证，或者用假的 `--provider local` 但没有真 Ollama 跑起来的情况下，确认失败模式是 `NetworkError` 类的清晰错误，不是 crash）。
- **真机验证（不进快测）**：装 Ollama、`ollama pull moondream`、`pzt eval <proj> --provider local --json` 跑一次真实评估，人工看结果是否合理——跟 `pzt eval --provider gemini/claude` 现在的验收方式（"能手敲运行、JSON 输出形状稳定"）同级别，属于任务分解里的基准测试任务，不属于自动化测试范畴。

## 八、任务分解（供后续 `writing-plans` 阶段细化，这里只列骨架顺序）

1. **`Provider::Local` 结构性铺垫**：枚举新增、`to_string`、`LocalModelConfig` 定义、`Settings` 两个新字段 + 落盘测试。不改 `request_json` 的行为，纯新增。
2. **`request_json` 三路分支 + `build_local_request`/`parse_local_response`**：`ai_test.cpp` 新增的对称用例覆盖，这一步结束时 `request_json(..., Provider::Local, fake_post, config)` 单测已经能过。超时不在这一步处理，见任务 5。
3. **`EvaluationFn`/`EvaluationWorker`/`request_evaluation` 的签名涟漪**：按第四节列的三层逐一改，同步修 `PZT_FAKE_EVAL` 假函数和 `evaluation_worker_test.cpp` 里现有假函数的签名——这是最容易漏改的一步，任务描述里要把受影响文件列全。
4. **CLI 接线**：`cmd_eval` 的 `--provider local`、`browse.cpp` 的 `resolve_ai_provider`，headless smoke 测试新增用例。
5. **真机基准测试**：装 Ollama，至少对比 `moondream` 和一个次选（比如量化 Qwen2.5-VL 小尺寸版本），跑真实照片评估，记录质量主观判断 + 耗时，据此校准 `Settings.ollama_model` 默认值和超时数值（这一步也决定 `Provider::Local` 的超时机制——是否需要把 `HttpPostFn` 改成四参数支持差异化超时，见第三节"超时"）。这一步的产出可能反过来触发对前四步默认值的小幅调整，不是纯验收。

## 九、风险与待确认问题

- **`moondream` 能不能胜任"三维度技术评分"这种相对抽象的判断任务**：Moondream 系列主要以"看图问答/生成简短描述"见长，不确定在"曝光/构图/对焦"这种需要一定摄影判断力的任务上质量如何——这正是任务 5 真机基准测试要验证的，如果质量明显不可用，`Settings.ollama_model` 默认值要换成任务 5 里测出来更好的候选，不是本文档能预判的。
- **JSON Schema 强约束模式**：当前设计选了宽松 `"json"` 模式（第二节已说明理由），完整 Schema 约束能进一步提升小模型的字段命中率，但要求打破 `request_json` 的任务无感知设计。如果基准测试发现 `"json"` 模式下字段缺失/类型错误的 `ParseError` 率明显偏高，值得回头重新评估这个取舍，不是这次直接做。
- **超时机制和数值都待任务 5 确定**：不只是"180 秒对不对"这个数字问题——`Provider::Local` 目前完全复用 Claude/Gemini 的硬编码 60 秒超时，要不要为它单独设置更长的超时、要不要区分冷启动和热启动，这些机制性决定本身也留到任务 5 跟真实数据一起做（详见第三节）。
- **Ollama 版本兼容性**：`format` 字段接受完整 JSON Schema 是较新版本 Ollama 才支持的能力（本设计虽然选择不用，但 `"format": "json"` 宽松模式本身的最低版本要求也需要在文档/README 里注明，避免用户装了旧版本 Ollama 却查不出为什么不工作）。
- **`EvaluationFn` 签名扩展的涟漪范围需要实施时仔细排查**：第四节已经点出三个已知受影响点（`request_evaluation`、`PZT_FAKE_EVAL` 假函数、`evaluation_worker_test.cpp` 假函数），但不排除还有其它测试文件里构造过 `EvaluationFn` 假函数、这次摸底没扫到——任务 3 开始前应该先做一次 `grep -rn "EvaluationFn"` 全仓库确认覆盖完整。
</content>
