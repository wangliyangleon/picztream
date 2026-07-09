# PicZTream (PZT) Milestone 3 工程设计文档（增量一：审美评分——主动触发）

## 背景

`docs/M3_PRD.md` 已经拍板了这次增量的产品行为：`pzt open` 里新增一个 vim 风格的 `:` 命令行，输入内容作为可选的"额外指引"（留空直接回车也能提交），拼进系统固定的评分模板里，异步调用 Claude 或 Gemini 拿一个 1-100 的审美评分外加一段简短文字点评，同一张图片重复请求要明确提示"处理中"而不是静默吞掉或重复调用，结果落库后不需要用户手动刷新就能在信息栏看到。按 `AGENTS.md` 的工程契约，具体的表结构、模块划分、接口签名要落到这份文档，实现应在评审通过之后再开始。

PRD 定稿之后追加的一条关键设计要求：**"发起一次带图片的 AI 请求"这一层要为未来复用而设计**。审美评分只是 M3 四个能力（自动粗修、审美评分、自动打标签、近似重复检测）里的第一个，以后自动粗修这类能力也要走同一套机制，只是期望模型返回的 JSON 字段不一样（比如 lightness/shadow 而不是 score）。这份文档因此把"AI 调用"拆成两层：一层完全不知道"score"是什么，只负责"发起请求、按调用方描述的形状要求 JSON、把解析出来的 JSON 原样交还"；另一层是审美评分自己的，建在通用层之上，知道要从结果里取哪个字段、失败了怎么算、要不要去重、存进哪张表的哪一列。

## 现有代码基础

三处直接影响这次设计的既有实现（不重复贴代码，只记结论）：

* **`core/browse/prefetch.h`/`.cpp` 的 `PrefetchCache`**：单个 `std::jthread`（成员声明在类的最后，保证它先于其它成员析构）+ `std::mutex` + `std::condition_variable_any` + 状态 map 的异步骨架；`get()` 是阻塞等结果的。这次的 `ScoreWorker` 照抄这个骨架（单线程 + 队列 + 条件变量），但接口必须是非阻塞轮询——AI 网络请求可能要几秒到十几秒，主线程绝不能等。
* **`core/db/database.h`/`.cpp` + `core/project/project.cpp` 的 `raw_preview_cache_dir()`**：默认数据库路径走 `$XDG_CONFIG_HOME/pzt/pzt.db`，任何需要"数据库同级目录"的东西（比如 RAW 预览缓存）都从 `db.path().parent_path()` 派生，不硬编码 `~/.config/pzt`——这样测试指向临时库时连带数据都落在临时目录。这次不需要新的配置文件（API key 走环境变量，见"技术选型"），但 `ScoreWorker` 的后台线程如果要开自己的数据库连接，要沿用"从 `db.path()` 派生"而不是重新猜路径的原则（这里其实更简单：直接调 `db::Database::open_default()`，跟现有 `core/api.cpp` 里每个门面函数各自开一次连接的写法一致，不需要额外派生逻辑）。
* **`cli/ui/ui.cpp` 的 `stdin_ready()`（`poll()` 包装）+ `cli/commands/browse.cpp` 的 `--debug` 用法**：只有 `debug_mode` 时，阻塞 `read()` 之前先 `poll(300ms)`，超时就跳回外层重绘刷新 debug 面板；不开 `--debug` 时维持纯阻塞 `read()`，没有 poll 开销。这次要新增一个不同性质的分支：AI 请求挂起时也要 poll，但**只有请求真的完成时才重绘**，不能照抄 debug 模式"每次超时都重绘"的逻辑，否则会变成没事就闪一下。

另外两处约束：`core/db/schema.cpp` 的 `ensure_column`/`column_exists` 是这次加列要复用的幂等迁移助手；`core/recipe/recipe.h` 里已经有过明确拒绝 JSON blob 列、坚持 typed column 的先例，这次的评分结果也遵循这条。

仓库里目前完全没有 HTTP 或 JSON 相关代码。

## 数据库 Schema 设计

`images` 表新增三列（复用 `ensure_column`）：

```sql
ALTER TABLE images ADD COLUMN ai_score INTEGER;
-- 审美评分结果，1-100 整数。可空——没评过分、或者评分请求失败，都落在
-- NULL，不是错误状态。

ALTER TABLE images ADD COLUMN ai_score_comment TEXT;
-- 跟 ai_score 同一次请求产出的简短文字点评，语言风格和字数限制由评分
-- 模板规定(见"core/api 接口设计"一节)，不是用户每次输入的东西。跟
-- ai_score 同生共死——评分请求失败时两列都保持 NULL，不会出现有分数没
-- 点评或者反过来的情况。

ALTER TABLE images ADD COLUMN ai_score_prompt TEXT;
-- 产出这次结果时用户输入的"额外指引"原文(可能是空字符串——用户直接回
-- 车、没输入任何额外指引也是一种合法输入，见 PRD"触发入口"一节)。这里
-- 存的是用户输入的原始片段，不是拼接了固定模板之后的完整 prompt——完
-- 整模板是代码里的常量，不需要每行都重复存一遍。

ALTER TABLE images ADD COLUMN ai_score_provider TEXT;
-- 产出这个结果用的是哪家供应商("claude" | "gemini")，供以后对比不同
-- 供应商效果、或者排查某次评分结果异常时用。
```

四列都可空、都是 typed column，不用 JSON blob 打包（呼应 `core/recipe/recipe.h` 的既有决定）——以后如果自动粗修也要类似的落库，会是各自独立的 typed column，不会把这四列的设计变成一个通用的"AI 结果"表。这次不引入这样的通用表，理由见下面"风险与待确认问题"。

`core::project::ImageInfo` 加四个对应的 `std::optional` 字段（`ai_score`/`ai_score_comment`/`ai_score_prompt`/`ai_score_provider`），`get_image()` 的 SELECT 补四列。

## core/api 接口设计

### `core/ai/ai.h`/`.cpp`：通用层，不知道"score"是什么

```cpp
namespace pzt::core::ai {

enum class Provider { Claude, Gemini };

enum class RequestError {
  MissingApiKey,  // 对应的环境变量没设(ANTHROPIC_API_KEY / GEMINI_API_KEY)
  NetworkError,   // curl 请求本身失败(超时/连不上)
  HttpError,      // HTTP 状态码非 2xx
  ParseError,     // 响应体不是预期结构,或者模型没按要求只回 JSON
};

// schema_instruction:调用方用自然语言描述这次要模型回什么样的 JSON(字
// 段名、类型、取值范围),会被拼进固定的系统层指令模板里,连同 user_prompt
// 一起发给模型。返回值是解析出来的 JSON 对象本身,由调用方自己按需要的
// 字段名去取——这个函数不假设、也不校验里面有什么字段。以后新增一种 AI
// 能力(比如自动粗修要 lightness/shadow)只需要调用方那一层换一段
// schema_instruction、换一套字段提取逻辑,这个函数和它背后的 HTTP/供应
// 商请求构建都不用动,这是"未来复用"这条要求的落地方式。
Result<nlohmann::json, RequestError> request_json(const decode::DecodedImage& image,
                                                    const std::string& user_prompt,
                                                    const std::string& schema_instruction,
                                                    Provider provider);

}  // namespace pzt::core::ai
```

内部按 `provider` 分发到两个供应商各自的请求体构建 + 响应解析：

* **Claude**（Messages API）：`messages[0].content` 是一个数组，一个 `image` 块（`source.type=base64, media_type=image/jpeg`）+ 一个 `text` 块（系统指令模板 + `schema_instruction` + `user_prompt` 拼在一起）；认证走 `x-api-key` header。响应体的 `content[0].text` 是模型的文本输出，这段文本本身需要再解析一次 JSON（这是"要求模型只回 JSON"这种简单方式的固有结构，不是 bug）。
* **Gemini**（generateContent API）：`contents[0].parts` 数组，一个 `inline_data`（`mime_type=image/jpeg`）+ 一个 `text` 块，认证走 URL query 参数 `key=`。响应体的 `candidates[0].content.parts[0].text` 是模型文本输出，同样需要再解析一次内层 JSON。

两者共用一个最小的 curl 封装（POST JSON body + header，写回调收响应体到 `std::string`），设置一个合理的请求超时（比如 60 秒）——不然后台线程可能因为网络问题无限期挂着，`ScoreWorker` 析构时要 `join()` 这个线程，超时上限直接决定了"退出程序最坏情况要等多久"。

**这次不接入两家供应商各自原生的结构化输出机制**（Claude 的 `tool_use` 强制 schema、Gemini 的 `generationConfig.responseSchema`）——那种方式更可靠（模型没法"忘记"按格式回答），但要分别学两套机制，这次先用"prompt 里用自然语言要求模型只回 JSON"这种更简单的方式把链路跑通。如果真机测试发现 `RequestError::ParseError` 频繁出现（模型经常不听话），再评估要不要升级；升级时 `schema_instruction` 这个参数会从一句自然语言换成结构化的 schema 对象，但 `request_json` 的整体形状、调用方的用法不需要大改。

### `core/ai/score.h`/`.cpp`：审美评分——`request_json` 的第一个消费者

评分任务本身（从哪些维度评价、给出 1-100 分数、附一段简短点评）由这一层定义，不是用户每次输入的——用户在 `:` 里输入的内容只是可选的"额外指引"，补充/调整这次评分的侧重点，见 `docs/M3_PRD.md`"触发入口"一节。这一层负责把"固定评分模板 + 用户的额外指引"拼成完整的任务描述，再交给 `request_json`：

```cpp
namespace pzt::core::ai {

enum class ScoreError { MissingApiKey, NetworkError, HttpError, ParseError, OutOfRange };

struct ScoreResult {
  int score;            // 1-100
  std::string comment;  // 简短文字点评，语言风格/字数限制由模板规定
};

// extra_guidance:用户在 `:` 里输入的原始文本，可能是空字符串(用户直接
// 回车，没有额外指引，这也是合法输入，不是错误)。内部先拼出完整的评分
// 任务描述——固定模板("evaluate this photo's aesthetics across color,
// composition, ……")后面跟一段"Additional guidance: {extra_guidance}"
// (extra_guidance 为空时省略这一段，不留一个空标签在 prompt 里)——作为
// user_prompt 传给 request_json；schema_instruction 相应描述两个字段
// (score、comment)。从结果 JSON 里取这两个字段、校验 score 落在
// 1-100——取不到字段、类型不对、或者 score 越界，都算失败，不写库。
// RequestError 直接映射到同名的 ScoreError，加一个 request_json 那层不
// 知道的 OutOfRange。模板本身是发给 AI 的系统层指令，不会展示给用户看，
// 固定用英文，不跟着 cli::i18n 的 zh/en 走（用户的额外指引本身可以是任
// 何语言，模型能处理，只是包住它的固定框架文案是英文）；具体措辞是实现
// 细节，这份文档不锁定逐字文本。
Result<ScoreResult, ScoreError> request_score(const decode::DecodedImage& image,
                                               const std::string& extra_guidance,
                                               Provider provider);

}  // namespace pzt::core::ai
```

以后如果要加自动粗修，会是同目录下平行的一个文件（比如 `core/ai/edit_suggestion.h`/`.cpp`），一样调用 `ai::request_json`，只是模板换成描述 lightness/shadow 之类字段的说法，返回类型换成对应的结构体——`ai.h`/`ai.cpp` 完全不用动。

### `core/ai/score_worker.h`/`.cpp`：异步 + 去重 + 落库（审美评分专属，不是通用层）

跟 `PrefetchCache` 同样的骨架（单 `jthread` 声明在最后、`mutex`+`condition_variable_any`、FIFO 队列、`request_stop`/`notify_all`/`join`），接口是非阻塞轮询：

```cpp
namespace pzt::core::ai {

class ScoreWorker {
 public:
  // 真正调用 AI 的函数可注入(照抄 M2 core::raw::RawDecodeFn 的依赖注入先
  // 例),默认指向真实的 ai::request_score;单元测试注入假函数,不需要真
  // 的连网络。
  using ScoreFn = std::function<Result<ScoreResult, ScoreError>(const decode::DecodedImage&,
                                                                 const std::string&, Provider)>;
  explicit ScoreWorker(ScoreFn score_fn = request_score);
  ~ScoreWorker();  // 析构 request_stop + notify_all + join,跟 PrefetchCache 一致

  ScoreWorker(const ScoreWorker&) = delete;
  ScoreWorker& operator=(const ScoreWorker&) = delete;

  // 发起一次评分请求,登记到队列末尾。extra_guidance 是用户在 `:` 里输
  // 入的原始文本，可以是空字符串。这张图已经在队列里或正在处理时,直接
  // 返回 false、不重复登记——调用方据此提示"AI 处理中,请稍后"。
  bool request(project::ImageId image_id, Provider provider, const std::string& extra_guidance);

  // 队列是否非空——驱动主循环要不要在阻塞 read() 之前先 poll(stdin)。
  bool has_pending() const;

  // 自 last_seen_generation 以来有没有请求完成(不管成功失败),有的话把
  // 最新值写回 last_seen_generation 并返回 true——用来判断这一轮 poll
  // 超时要不要触发一次真正的整屏重绘,不是每次超时都重绘造成"没事就闪
  // 一下"。内部一个单调递增计数器,每完成一个请求(无论成功失败)+1。
  bool consume_new_result(std::uint64_t& last_seen_generation) const;
};

}  // namespace pzt::core::ai
```

worker 线程处理一个请求时：自己开一个 `db::Database::open_default()` 连接（不跨线程共享主线程的连接——`PrefetchCache` 至今没有跨线程碰数据库的先例，各开各的连接是最简单、不引入新线程安全问题的做法），查 `project::get_image` 拿 `file_path`/`project_id`，查项目 `root_path` 拼出绝对路径，调 `core::decode_preview_file` 解码（跟浏览复用同一条路径，不单独再解码一份），调注入的 `score_fn`，成功就 `UPDATE images SET ai_score=?, ai_score_comment=?, ai_score_prompt=?, ai_score_provider=? WHERE id=?`（`ai_score_prompt` 存的是 `extra_guidance` 原文，不是拼接了模板之后的完整 prompt，见"数据库 Schema 设计"一节），失败就四列都不写（保持 NULL）。两种情况都要把这张图从"进行中"标记里摘掉、计数器 `+1`。

这一层是审美评分专属的，不是通用层——"要不要去重""结果存哪一列""怎么判断处理中"这些是这个具体能力的业务逻辑，不是所有 AI 能力都会长一样的形状（比如自动粗修的结果可能需要立即触发重新渲染预览，而不是单纯写库等下次重绘）。等真的有第二个 AI 能力落地时，再看这个 `jthread`+队列+去重+generation 计数器的骨架要不要提炼成一个通用模板/基类，这次不提前抽象。

### `core/decode`：新增内存态 JPEG 编码

现有 `encode_jpeg_file(DecodedImage, path)` 只写文件；M2 加过反向的 `decode_jpeg_bytes(bytes) -> DecodedImage`（RAW 内嵌预览解码用）。这次要把预览图编码成 JPEG 字节直接塞进 HTTP body（base64），不想为了传给 AI 单独落地一个临时文件，补一个对称的：

```cpp
std::vector<std::uint8_t> encode_jpeg_bytes(const DecodedImage& image);
```

## CLI 接线

**`cli/ui/ui.h`/`.cpp`**：新增一个 `read_text_line` 的变体，用于"占位提示，输入即替换"这种交互——现有的 `read_text_line(prompt, ...)` 是"字段标签"语义（"新建标签名: "这种要一直挂在输入内容前面的前缀，不会消失），跟这次 `:` 要的"提示怎么用、但输入内容本身不需要一直带着标签"不是一回事，不能直接复用：

```cpp
// 跟 read_text_line 类似,但 placeholder 是"占位提示"不是"字段标签"——
// buffer 还是空的时候整行显示 placeholder,用户一开始输入(buffer 非空)
// placeholder 就整个让位给 buffer 本身,不再当前缀混在一起。Esc 仍然返回
// nullopt(取消);buffer 为空时按 Enter 返回空字符串(不是取消)——调用方
// 决定空字符串是不是合法输入，这次 `:` 的场景里空字符串是合法的(表示
// "不需要额外指引")。
std::optional<std::string> read_text_line_with_placeholder(const std::string& placeholder,
                                                             int banner_row, int start_col,
                                                             int content_cols);
```

内部实现基本是 `read_text_line` 的翻版，只改 `redraw` 那一行：`buffer` 为空时画 `placeholder`，否则直接画 `buffer` 本身（不拼前缀）。

**`cli/commands/browse.cpp`**：

* `ScoreWorker` 实例的生命周期跟 `PrefetchCache` 一样，声明在 `cmd_open` 的同一个块里（`AltScreen`/`CbreakMode` 内部），程序退出时自然析构（析构会等还在跑的请求完成——这是 `jthread` 正确管理生命周期的直接代价，接受这个行为：用户主动发起过评分再退出，等一下是合理的，不做 detach 之类放弃生命周期管理的取巧方案）。
* 接受键集合加 `':'`。处理逻辑：`read_text_line_with_placeholder(msg_ai_prompt_placeholder(), banner_row, start_col, content_cols)`；`nullopt`（Esc）静默取消；否则不管拿到的字符串是空还是非空、是否以 `/` 开头都要往下走一步判断——`/` 开头这次不处理，直接静默忽略（PRD 已经明确这次不定义行为，只留分支占位）；其余情况（含空字符串）调 `score_worker.request(current_ref->id, Provider::Claude, text)`——返回 `false` 时 `status_override` 提示"AI 处理中，请稍后"，返回 `true` 时给一句简短确认，不等结果、不阻塞。
* 内层读键循环的 poll 逻辑扩展：现在只有 `debug_mode` 才会 `stdin_ready(300)` 轮询；这次加一个 `score_worker.has_pending()` 分支——有请求在跑时也 `stdin_ready(300)`，但超时之后**先检查 `consume_new_result(...)` 有没有真的变化**，没有就继续内层循环再等（不触发外层重绘、不算一次超时事件），有变化才当作超时事件 `break` 出去重绘。这是满足"poll 重绘只在真正需要时才发生"的关键点。
* 信息栏（metadata block）在"风格"那一段之后加两行，复用现有 `emit_line` 的越界裁剪逻辑（同一个 `meta_bottom_row` 边界）：`AI 评分: 85` / `AI 评分: -`（没有分数时），紧跟一行 `AI 点评: <comment>`（没有时同样显示 `-`；`comment` 本身的长度已经被模板限制过，正常情况下不需要额外的多行换行处理，太长时按现有 `pad_to` 的截断行为处理，不需要跟"拍摄时间"那样特别做标题行+缩进值行）。

**`cli/i18n`**：新增 `msg_ai_prompt_placeholder()`（"输入额外指引，或直接按回车提交 AI 处理"）、`ai_score_label(std::optional<int> score)`、`ai_score_comment_label(std::optional<std::string> comment)`、`msg_ai_score_pending()`（"AI 处理中，请稍后"）、`msg_ai_score_submitted()`（提交确认）。

## 技术选型

新增两个只在 `core/ai/` 内部使用、不泄漏到 `cli/` 的依赖：

* **libcurl**：`pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)`，链接 `PkgConfig::CURL`——照抄 `core/CMakeLists.txt` 里 LibRaw 的接入方式（Homebrew 的 `curl` formula 是 keg-only，跟当年 `libomp` 一样，需要的话手动补 include/lib 路径，第一个 increment 里第一件事就是验证这个）。
* **nlohmann_json**：`find_package(nlohmann_json REQUIRED)`，链接 `nlohmann_json::nlohmann_json`——Homebrew 有现成的 CMake config package。这次不像 `doctest` 那样手动 vendor 进 `third_party/`——`doctest` 只在测试可执行文件里用，vendor 一份单头文件成本很低；`nlohmann_json` 是生产代码的运行时依赖，走系统包管理器更符合这个项目目前所有第三方运行时依赖（SQLite3、LibRaw）的既有模式。

**API key 走环境变量**（`ANTHROPIC_API_KEY`、`GEMINI_API_KEY`），不新增配置文件——这是两家供应商官方 SDK/文档里最常见的约定俗成的变量名，用户很可能已经设置过；比起设计一份新的配置文件格式（还要考虑存放路径、读写、权限），环境变量是这次范围内最省事且足够用的方案。`request_json` 内部读不到对应变量时直接返回 `RequestError::MissingApiKey`，不尝试其它兜底来源。

## 任务分解

1. **CMake 接线**：curl + nlohmann_json 能编译链接进 `core`，`core/ai/` 空壳能跑通，验证 Homebrew 的 keg-only 路径问题（同 LibRaw 先例）
2. **`core/decode::encode_jpeg_bytes`** + **`core/ai/ai.h`/`.cpp`**（`request_json` 通用层，两个供应商的请求/响应映射、curl 封装）：单元测试用假 HTTP 响应（不连真实网络）验证 JSON 解析、错误分类逻辑
3. **`core/ai/score.h`/`.cpp`**（`request_score`，`request_json` 的第一个消费者）：单元测试验证"固定模板 + extra_guidance 拼接"（含 extra_guidance 为空时不留空的"额外指引："这个边界）、score/comment 两个字段的提取、score 越界校验
4. **Schema 四列** + `ImageInfo`/`get_image` 扩展
5. **`core/ai/score_worker.h`/`.cpp`**（`ScoreWorker`）：注入假 `ScoreFn`，单元测试覆盖去重（同一张图请求中再次 `request()` 返回 `false`）、`consume_new_result` 的变化检测（完成前 `false`、完成后一次 `true`）、成功/失败两种情况的落库结果
6. **CLI 接线**：`:` 键、poll 重绘逻辑、信息栏展示——这一步交互本身没有自动化覆盖（沿用项目"cbreak 按键循环没法自动化测试"的一贯限制），靠真机验证
7. **真机验收**（需要真实 `ANTHROPIC_API_KEY`/`GEMINI_API_KEY`）：真实调通两个供应商，过一遍 `docs/M3_PRD.md` 的验收标准

## 风险与待确认问题

延续自 `docs/M3_PRD.md`（云端 vs 本地取舍、自动介入触发条件、其它三个 M3 能力、多供应商切换），这次工程设计阶段不展开，新增两条这个阶段才浮现的：

* **`schema_instruction` 用自然语言而不是原生结构化输出**：如果真机测试发现模型经常不按要求回 JSON（`RequestError::ParseError` 频繁），需要重新评估要不要升级成 Claude `tool_use` / Gemini `responseSchema`，见"core/api 接口设计"一节的说明
* **要不要一张通用的"AI 结果"表**：这次 `ai_score`/`ai_score_comment`/`ai_score_prompt`/`ai_score_provider` 是 `images` 表上的具体列，不是一张 `ai_results(image_id, capability, result_json)` 这样的通用表。等自动粗修真的要落地时，如果发现多个能力的存储需求高度相似（都是"某张图 + 某种能力 + 一次结果 + prompt + provider"），可能值得重新评估要不要抽出一张通用表；这次的具体列方案对应"只有一个已知消费者"的现状，不提前为假设中的第二个消费者设计存储层
