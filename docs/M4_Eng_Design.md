# PicZTream (PZT) Milestone 4 工程设计文档（增量一：粗筛交付最小闭环 + Telegram 接入）

## 背景

产品需求见 `docs/M4_PRD.md`（增量一：Telegram 收图 → 云端评估 → 去重 → 策展挑 N 张 → 发回 keeper）；编排骨架的设计语义见 `docs/M4_Agent_Workflow_Design.md`（Stage/Plan/Run、确定性驱动器、闸门、状态机、错误恢复、测试哲学）。**本工程设计不重复那两份的内容**，只回答"这套东西具体怎么落地"：模块与文件结构、跨语言边界、headless 命令契约、`Curate` 新算法、`agent/` 的接口签名、构建集成，以及一份**按依赖排序、每步可独立验证的子增量分解**。

关键工程前提（已在 spec 拍板）：形态 B——`agent/` 是独立 Python 进程，经一组 headless `pzt` 子命令（JSON 进出、子进程调用）驱动 C++ 的 `core`；`core` 对编排/策略/LLM 三重无感知；传输 v1 = Telegram 官方 Bot API。

## 现有代码基础（可直接复用，不改行为）

- **`core/api.h` 门面**：`create_project` / `list_images` / `get_image`（含 `evaluation`）/ `evaluated_image_ids` / `find_and_tag_duplicates` / `find_tag_by_name` / `images_with_tag` / `add_tag` / `ensure_reject_tag` / `export_images` / `passes_gate` / `overall_score` / `load_settings`——增量一要用的 core 能力**全部已存在**，缺的只是把它们暴露成非交互命令。
- **M3 的 provider 缝**：`core/ai/ai.h` 的 `Provider {Claude, Gemini}` + 可注入 `HttpPostFn`；增量一评估只走云端，`Provider` 不动（`Provider::Local` 属后续增量）。
- **M3 的注入即可测先例**：`HttpPostFn` / `EvaluationFn` / `RawDecodeFn` / `DecodeFn` 到处依赖注入——`agent/` 侧全部沿用这套哲学（假子进程 runner / 假传输 / 假 LLM 响应）。
- **`core/export/export.cpp` 的 `export_images`（M4 反馈轮已加）**：按 image_id 列表导出，正是 `Deliver` 需要的。
- **缺口**：这些能力目前几乎只经 `cli/` 交互流（`pzt open` 的菜单/控制台）触发；增量一要补一层**非交互命令面**（下一节）。

## 一、总体结构与新增模块

```
core/            (C++，不改架构；新增 Curate)
cli/             (C++，现有 `pzt open` + 新增一批 headless 子命令)
agent/           (新增，Python 包，独立 venv)
  transport/       传输适配器（telegram / watchfolder，统一接口）
  orchestrator/    Stage/Plan/Run 数据类型 + 确定性驱动器
  stages/          各 Stage 实现（shell out 调 pzt 子命令）
  compose/         意图/调整的 LLM 翻译器 + Plan 校验
  store/           agent 自己的 Run/Plan 持久化
  pzt_client.py    封装"子进程调 pzt 子命令、JSON 进出"的唯一入口
```

**边界机制**：`agent/` 通过 `pzt_client.py` 一个薄封装，`subprocess` 调 `pzt <subcommand> --json …`，stdout 收 JSON、非零退出码 + stderr JSON 表错误。**这是 `agent → cli → core` 单向依赖的唯一通道**，`agent` 不链接任何 C++，`core` 不知道 `agent` 存在。选子进程而非 FFI/RPC：agent 非延迟敏感、子进程可手敲复现、边界是纯 JSON 契约、最好测（spec 第七节的取舍）。

## 二、headless 命令面设计

给 `cli/` 增加一组非交互子命令（`cli/commands/` 里跟现有 `cmd_export` 等平级的薄壳），统一约定：

- `--json` 输出结构化结果到 stdout；失败时非零退出 + stderr 一行 JSON `{"error": "<code>", "message": "<人读>"}`（`<code>` 直接用 core 的 `Result<T,E>` 错误枚举名序列化）。
- 图片按 **`image_path`（项目内相对路径）** 寻址，内部 `find_image_by_path` 转 `image_id`（路径对人和脚本都稳定）。
- 交互流里藏的"人的决定"（如 `add_tag` 超 cap 的替换子菜单）在命令侧变成**显式策略参数**（如 `--on-cap {fail|skip}`），不弹菜单。

增量一需要的子命令（都是已有 `core/api` 的薄壳，`Curate` 除外见第三节）：

| 子命令 | 映射 core | 关键参数 / 输出 |
|---|---|---|
| `pzt new <name> <folder>` | `create_project` | 已有；输出加 `--json`（project 名 / 图片数）——跟其它 headless 命令一致按项目名寻址，不返回内存态的 `project_id`（同一个理由见 `find_image_by_path`：名字/路径对人和脚本都比数据库内部 id 稳定） |
| `pzt images <proj> --json` | `list_images` + `get_image` | 列出图片：path / 是否已评估 / passes_gate / 标签。agent 读状态用（复用 `evaluated_image_ids` 批量查，避免 N+1） |
| `pzt eval <proj> --scope <*|#tag> --provider <gemini\|claude> [--auto-reject] --json` | `EvaluationWorker` 批量 | `--auto-reject`（策略参数，agent 默认传）：不达标即 `add_tag(reject)`，**显式传参、不读也不改 `Settings.auto_ai_reject`**（物理隔离，PRD P6）。同步跑完（批处理场景不需要异步 worker 的非阻塞语义，直接顺序评估+等待）；输出每张 结果/跳过/失败 |
| `pzt dedup <proj> --scope <*|#tag> --json` | `find_and_tag_duplicates` | 时间窗/阈值读 `Settings`（M3 F-08 已支持）；输出组数/标记数/skipped_no_capture_time |
| `pzt curate <proj> --count N [--tag <name>] [--apply-tag <name>] --json` | 新增 `core::curate`（第三节） | `--tag` 是候选范围限定（可选，缺省整个项目），跟 `--apply-tag`（落到入选图上的标签，可选，默认"精选"）是两个独立概念。分簇 threshold 读独立的 `Settings.curate_time_window_seconds`/`curate_hash_threshold`（比 `dedup_*` 默认更宽松，见第三节）；输出选中的 image_path 列表 + 落一个可配置的普通标签（惰性建，非系统标签，重复运行不清历史标记——见第三节） |
| `pzt tag apply <proj> <image_path> <tag> [--on-cap {fail\|skip}] --json` | `add_tag`（+ `create_tag` 惰性建） | 幂等 |
| `pzt tag clear <proj> <tag> --json` | `filter_by_tag` + `remove_tag` | 把项目里当前打了这个标签的所有图整体摘掉（无 `--scope`，范围就是整个项目）；标签不存在是幂等成功（`cleared:0`），不报错。给 agent 用的清场命令——`pzt curate` 的 `--apply-tag` 重复运行不清历史标记（见第三节），想要"这次是全新一批"的语义就先调这个命令清掉上一轮的标签，再重新 `curate` |
| `pzt export-images <proj> <image_path…> <folder> --json` | `export_images` | 输出导出/跳过清单 + 目标路径 |
| `pzt export-images <proj> <image_path…> <folder> --json` | `export_images` | 输出导出/跳过清单 + 目标路径 |

**为什么 `pzt eval` 在命令侧同步而不复用异步 `EvaluationWorker` 的非阻塞轮询**：`EvaluationWorker` 的非阻塞 + `generation_` 轮询是为 `pzt open` 交互式主循环设计的（不阻塞按键）；命令行批处理里没有交互主循环要保护，一条命令就该"跑完这批再返回"。实现上直接复用 `EvaluationWorker`（不新写一份同步评估函数）：逐张 `request()` 提交，轮询 `queue_status()` 直到 `queued==0 && !processing` 判定收尾，不用 `consume_new_result()` 的世代号计数——世代号只回答"有没有新结果"，解码失败这类不用等网络的请求可能在两次 poll 之间就连续完成好几个，世代号会被观测成一次变化，用它数"还剩几个没完成"会数少、永远等不到 0。对外契约是"同步跑完、输出每张 结果/跳过/失败"；进度**不做**结构化 JSON 流，沿用 `EvaluationWorker` 已有的 stderr 人读日志（`[pzt ai] evaluation worker: ...`）——增量一批量通常是几十张、单条命令跑完通常几分钟内，暂不需要专门的进度协议，agent 侧靠命令本身的阻塞返回感知"跑完了"，需要更细粒度进度回报是留到以后的开放问题，不在这次范围内。

## 三、Curate 算法设计（增量一唯一的新算法）

落 `core`（确定性照片分析，归 core 侧；`core/curate/curate.h`/`.cpp`），经 `pzt curate` 暴露。

**输入**：一个 project 的候选范围（默认全项目，或 `--tag` 限定）+ 目标张数 `N`。
**候选集**：`passes_gate(evaluation)` 为真 ∧ 非"废片" ∧ 非"重复"（复用 `get_image` 的 `evaluation` / `passes_gate` / `images_with_tag`）。未评估的图**不进候选**（增量一策略：没评过就不参与自动挑选；agent 的 Evaluate Stage 保证候选前都评过）。
**目标**：从候选里挑 N 张，**兼顾质量与多样性**——不能把同一场景/连拍的近重复塞满 N 张。

**算法（确定性、可复现，不依赖语义 embedding——那是本地 Vision，属后续增量）**：

1. **分簇**：复用 dedup 的分簇算法（`dedup::find_duplicates`）在候选集合上**现场重新分簇**，不是读取历史分组——候选集已经排除了"重复"标签图片（每组只剩 `keep_id` 那一张），`find_and_tag_duplicates` 也不落库分组关联，上一轮 dedup 跑完分组信息就没了，没有旧状态可读。`curate` 直接调纯算法 `find_duplicates(db, root_path, candidate_ids, time_window_seconds, hash_threshold)`，候选里被分进同一个 `DuplicateGroup` 的视为同一"视觉簇"，未进任何组的各自成簇。
   **阈值不能复用 `Settings.dedup_time_window_seconds`/`dedup_hash_threshold` 原值，必须比 dedup 标记阈值更宽松（更容易合并）**：候选集里能剩下的图，按定义就是上一轮 dedup 用这组阈值判断"不够像"、没被标记重复的图——如果分簇再用同一个（或更严格/更窄的）阈值，候选池里的图幾乎不会被合併，每张各自成簇，分簇退化成"没分"，第 3 步的"跨簇挑 N"就名存实亡，起不到多样性保护作用（同一场景不同角度、没触发 dedup 阈值的几张图可能同时挤进最终 N 张）。只有用比 dedup 更宽松的阈值重新分簇，才能把"没被判成重复、但视觉上仍然明显相似"的图归进同一簇，逼跨簇挑选只从中选一个代表，真正实现多样性。
   落地：新增 `Settings.curate_time_window_seconds`/`curate_hash_threshold`（独立于 `dedup_*`，不共用同一份配置，避免"调宽了 curate 顺带影响 dedup 标记"这种耦合），初始默认值取 dedup 默认值的 2 倍（`dedup_time_window_seconds`/`dedup_hash_threshold` 默认 10s/5 → curate 默认 20s/10），是否合适留给真机使用后按效果调整，不是精确调出来的数字。同 `find_duplicates` 本身的既有约定（`core/dedup/dedup.h` 99-103 行）：**`core::curate::curate` 不直接读 Settings**，由调用方（`pzt curate` 命令）读 `Settings.curate_time_window_seconds`/`curate_hash_threshold` 后显式传参，`curate()` 签名加 `time_window_seconds`/`hash_threshold` 两个参数（见下方 core API），不新增 CLI flag。（增量一不额外算 embedding，直接吃 dedup 分簇算法当"相似"的免费信号，只是阈值調松。）
2. **簇内选代表**：每簇取 `overall_score` 最高的一张当代表（并列时 `captured_at` 更新的、再并列 image_id 最小，跟 dedup `pick_keep_id` 同一套确定性 tie-break）。
3. **跨簇挑 N**：
   - 若簇数 ≥ N：按代表的 `overall_score` 降序取前 N 个簇的代表——保证 N 张来自 N 个不同视觉簇（多样性优先）。
   - 若簇数 < N：先每簇一张代表（覆盖所有场景），剩余名额再从各簇的次优图里按 `overall_score` 补齐（质量优先补位），直到 N 张或候选耗尽。
4. **时间铺开（tie-break 层面）**：跨簇挑选在 `overall_score` 相等时，优先让入选集的 `captured_at` 分布更散（简单实现：相等分数下，选与已入选时间差最大的），让 N 张更像"一天的故事"而非"扎堆一个时段"。
5. **候选不足 N**：返回全部候选并在输出里标 `requested=N, returned=M`（M<N），不报错——交给 agent/用户知晓。

**core API**：
```cpp
// core/curate/curate.h
namespace pzt::core::curate {
struct CurateResult {
  std::vector<project::ImageId> selected;   // 有序:入选顺序(便于"第1张/第2张"引用)
  int requested;
  int returned;                             // == selected.size()，< requested 表示候选不足
};
// candidate_scope 为空 => 全项目;否则限定到某标签下。N > 0。
// time_window_seconds/hash_threshold：分簇复用的 find_duplicates 参数，
// 调用方从 Settings.curate_time_window_seconds/curate_hash_threshold(独
// 立于 dedup_*，默认值更宽松)读出显式传入(curate 本身不读 Settings，跟
// find_duplicates 同一约定)。
CurateResult curate(db::Database& db, project::ProjectId project_id,
                    std::optional<TagId> candidate_scope, int count,
                    int time_window_seconds, int hash_threshold);
}
```
`curate` 只做**选择**，不打标签、不导出（单一职责）；`pzt curate` 命令在拿到结果后落一个**可配置的普通标签**（`--apply-tag <name>`，默认"精选"，惰性建/`find_tag_by_name` 找不到就 `create_tag(..., is_system=false)`，非系统标签，重复运行不清历史标记）——不是固定的系统标签：用户如果要挑"朋友圈"/"ins"投稿这类场景，标签名字应该直接就是"朋友圈"/"ins"，不该被强绑成"精选"，走的是跟 `pzt tag apply` 完全一致的惰性建标签路径（`cli/commands/commands.cpp` 的 `resolve_or_create_tag`，两处共用）。确定性：同库同参数同输出，直接单元测试。想要"这次是全新一批"的语义（清掉上一轮的标签再重新挑），调用方自己先调 `pzt tag clear <proj> <tag> --json`（见第二节命令表）清场，不是 `curate` 自己的职责。

## 四、agent/ 结构设计（Python）

设计语义见 spec；这里定接口边界与模块职责。内部实现细节（每个 Stage 的具体解析、每条测试）留给各子增量的实现计划。

**orchestrator/（Stage/Plan/Run + 驱动器）**：
```python
# 数据类型(dataclass, 可 JSON 序列化)
class StageStatus(Enum): PENDING; RUNNING; DONE; FAILED; SKIPPED
@dataclass StageSpec:  name: str; params: dict; gate: Literal["off","courtesy","required"]
@dataclass Plan:       stages: list[StageSpec]         # 有序
@dataclass RunState:   run_id; project_id; plan: Plan; stage_states: dict[str,StageStatus];
                       outputs: dict[str, Any]; gate_state: dict; intent_raw: str; status: RunStatus

# Stage 抽象:每个 Stage 实现一个纯函数式接口
class Stage(Protocol):
    name: str
    inputs: list[str]        # 依赖的前置 Stage 名
    cost_class: Literal["local","cloud"]
    criticality: Literal["critical","optional"]
    def run(self, ctx: StageContext, params: dict) -> StageOutput: ...

# 驱动器:纯确定性,零 LLM
class Driver:
    def __init__(self, stages: dict[str,Stage], store: RunStore, transport: Transport): ...
    def advance(self, run: RunState) -> RunState: ...   # 走一步;闸门/失败/完成分别转对应状态
    def apply_adjustment(self, run: RunState, delta: PlanDelta) -> RunState: ...  # 作废受影响子图+重跑
```
驱动器只认 `Stage` 抽象——测试注入**假 Stage**（返回预设 `StageOutput`）即可覆盖依赖顺序/闸门/失败降级/检查点/续跑/子图重跑，不碰真命令、真模型、真传输（spec 第九节，最大测试回报）。

**stages/**：`Ingest` / `Evaluate` / `Dedup` / `Curate` / `Deliver`，每个 `run()` 内部经 `pzt_client` 调对应子命令（第二节）。`pzt_client` 可注入假 runner → Stage 单测不需要真 `pzt`。

**compose/**：`compose_plan(intent, profile, last_config) -> Plan`（LLM）与 `parse_adjustment(msg, run) -> PlanDelta`（LLM）；两者输出都过一道**确定性校验** `validate_plan(plan) -> Plan | Error`（Stage 存在、参数在范围、依赖可满足）。校验是纯逻辑，硬测；LLM 部分注入假响应测映射 + 离线 eval 集。

**transport/**：
```python
class Transport(Protocol):
    def receive(self) -> Iterator[InboundMessage]: ...   # 文本 / 文件(原图) / 照片
    def send_text(self, chat, text): ...
    def send_photo(self, chat, path): ...                # 预览:压缩
    def send_file(self, chat, path): ...                 # 交付/原图:字节精确
```
`TelegramTransport`（`python-telegram-bot`）与 `WatchFolderTransport`（开发/测试期等价传输）。会话路由（消息→Run、状态优先归类）在驱动器之上一层。测试用**假内存 Transport** 跑全对话流。

**store/**：`RunStore`——agent 自己的持久化（SQLite 或 JSON 文件，按 `run_id`，引 core 的 `project_id`），`save(run)` / `load(run_id)` / `list_active()`（重启后续跑）。**绝不进 core 的 `pzt.db` schema**。

## 五、测试策略落地

- **core（C++）**：`core::curate` 用 doctest 单测（候选过滤、分簇选代表、簇数≥N/<N 两分支、候选不足、tie-break 确定性）；headless 子命令加薄 CLI smoke（跑一次断言 JSON 形状）。core 其余不动，保持既有测试。
- **agent（Python，pytest）**：驱动器（假 Stage：顺序/闸门/降级/检查点/续跑/子图重跑）；`validate_plan` 硬测；compose 的 LLM 部分注入假响应 + 离线 eval 集；Stage 用假 `pzt_client` runner；全对话流用假 Transport。
- **端到端**：`WatchFolderTransport` 天然一条真集成测（丢真照片 → 真 `pzt` 子命令 → 真云端评估 → 真产出），慢、跑得少；`TelegramTransport` 按项目惯例走真机手动验证（像 cli 的 pty 验证）。

## 六、技术选型与构建集成

- **Python**（agent/，独立 venv，`pyproject.toml`）；**`python-telegram-bot`**（成熟、异步、官方 Bot API）；**`subprocess`** 调 `pzt` 子命令（JSON 契约）；LLM 客户端用各家官方 SDK 或直接 HTTP（意图/调整解析走云端，同 M3 的 provider）。
- **构建**：`core`/`cli` 仍走现有 CMake + Ninja（`build/` Debug、`build_release/` RelWithDebInfo）；`agent/` 独立 Python 工具链，不进 CMake。`pzt_client` 通过环境变量/配置拿到 `pzt` 可执行文件路径（默认指向 `build_release/cli/pzt`）。多语言仓库，两套测试各自跑（doctest + pytest）。
- 不引入运行时重框架（守 AGENTS.md）；agent 的编排逻辑保持轻量、确定。

## 七、任务分解（按依赖排序，每个子增量是"能独立跑通并验证"的可交付单元）

每个子增量结束时都有独立可验证的交付物；**每个子增量在真正实现时再各自出一份 bite-sized 实现计划**（`superpowers:writing-plans`）。A、B 都是 C++、可并行；C 是纯 Python、独立于 A/B。

1. **子增量 A — headless 命令面（C++）**：第二节那批 `pzt` 子命令（`images`/`eval`/`dedup`/`tag apply`/`export-images` + `new --json`）。
   *验证*：每个命令手敲跑通、JSON 形状稳定的 CLI smoke 测；`pzt eval --auto-reject` 不改全局 `Settings`（查配置未变）。*交付*：core 全部能力可经命令行 + JSON 驱动。
2. **子增量 B — `Curate`（C++ core + 命令）**：`core/curate` + `pzt curate`。
   *验证*：doctest 覆盖候选过滤/分簇/两分支/候选不足/确定性；`pzt curate --json` smoke。*交付*：给定库能确定性选出 N 张多样 keeper。
3. **子增量 C — agent 骨架（Python，假 Stage）**：`orchestrator/`（Stage/Plan/Run + 驱动器）+ `store/`。无真命令、无传输、无 LLM。
   *验证*：pytest 用假 Stage 覆盖依赖顺序、三态闸门、失败降级/中止、每 Stage 检查点、**模拟崩溃续跑**、**调整→子图作废重跑**。*交付*：确定性编排内核，脱离一切外部依赖可测。
4. **子增量 D — 真 Stage + watch-folder 端到端（Python）**：`stages/` 五个 Stage 经 `pzt_client` 接子增量 A/B；`WatchFolderTransport`。先固定 Plan（不含 LLM），跑通"丢文件夹 → 云端评估 → 去重 → 策展 → 导出"。
   *验证*：Stage 用假 runner 单测；watch-folder 真端到端（真照片、真云评估、真产出）；`Deliver` 幂等（模拟"发了未标 done"崩溃不重发）。*交付*：可跑的无 IM 粗筛闭环。
5. **子增量 E — 意图/调整 composer（Python LLM）**：`compose/` 的 `compose_plan` / `parse_adjustment` + `validate_plan`；接上驱动器（自然语言驱动 Plan、对话调整触发子图重跑）。
   *验证*：`validate_plan` 硬测；LLM 映射注入假响应测 + 离线 eval 集；接驱动器的"意图→跑、调整→重跑子图"用假 Transport 端到端。*交付*：一句话意图能驱动整条流水线、对话能微调。
6. **子增量 F — Telegram 接入（Python）**：`TelegramTransport` + 会话路由（状态优先归类、触发/静默问询、预览照片/交付文件两段式、并发问询）。
   *验证*：全对话流用假 Transport 单测；`TelegramTransport` 真机手动验证（真 bot：发文件 + 意图 → 收 keeper 文件 + 摘要；换掉某张/改张数微调；断点续跑）。*交付*：增量一完整最小可用闭环（Telegram v1）。

到子增量 D 结束已有可用的无 IM 粗筛闭环；到 F 结束即 PRD 的 Telegram v1。

## 八、风险与待确认问题

- **`Curate` 多样性的效果上限**：增量一不做语义 embedding，多样性只吃 dedup 分组 + 时间铺开 + 分数。dedup 只抓"近重复"，抓不到"两张不同但都是逆光剪影"这种语义相似——真机观察是否够用，不够就等后续增量的 Tier 0 `VNGenerateImageFeaturePrint` 增强。
- **`pzt eval` 同步批处理的时长**：几十张云端评估顺序跑可能几分钟，命令会阻塞这么久，且这次没做结构化进度协议（见第二节），agent 侧目前只能整体等命令返回，体感上是个黑箱。若真机嫌慢/嫌体感差，考虑命令内并发几路请求（但要守 M3 的 provider 限流/超时，F-21）或者后续增量给 `pzt eval` 补一版结构化进度输出——都不在这次范围内。
- **Python↔C++ 版本/路径耦合**：`pzt_client` 依赖 `pzt` 可执行文件路径与 JSON 契约稳定；子命令的 JSON 输出形状要当成对 agent 的公开 API 维护，改动需同步两侧。用一版契约测试（agent 侧存几个期望 JSON、CLI 侧 smoke 产出）锁住。
- **LLM 组装 Plan 的可靠性**：意图/调整解析走云端，非确定；靠 `validate_plan` 确定性护栏兜底 + 离线 eval 集抓 prompt 回归。真机观察组装准确率。
- **Telegram 文件收发上限**：默认 bot 收 20MB / 发 50MB，手机 JPEG 够；RAW/大文件要自搭 local Bot API server（2GB），属后续。
- 延续 PRD 的 TODO（本地模型、完整朋友圈 Style/Caption、多 Profile、其它 IM）不在本增量。
