# 导航图

只记**搜索会答错或答不出**的东西：命名看不出的归属、间接层真正落在哪、散在多个文件里的流程、以及会咬人的陷阱。目录树与文件清单不在这里 - `ls` 和 `grep` 答得更准，而且永远不过期。

分层契约与模块清单见 `AGENTS.md`；接口轮廓见 `docs/SPEC.md` 第三节。

## 名字骗人的地方

- **`core/api.h` 是门面，不是一个模块目录。** 它和 `core/<模块>/` 平级但只是两个文件（`api.h`/`api.cpp`），把各模块包成 cli 能调的一层。它**每次调用都 `Database::open_default()` 开一次库** - 单次约 104µs，逐张循环调用时会放大。已经有两处为绕开它而在 core 加了专用批量接口（`recipe::render_preview`、`recipe::count_images_with_recipe`），再遇到"要对 N 张做同一件事"时先看能不能加第三个，而不是循环调门面。
- **`core/ai/style.h` 管的是"看图选风格"这个 AI 动作，不是配方数据结构。** 配方本体在 `core/recipe/`。
- **`agent/router/` 只剩 `collecting.py` 一个纯函数模块。** 旧的单线程 `session_router` 已删，会话逻辑全在 `agent/session/`。看到 `router` 不要以为那是入口。
- **`cli/compare/` 是两图对比选片（人工锦标赛）**，跟 `core/ai/compare.h` 的 AI 两两比较是两回事，只是同名。
- **`agent/build/` 是打包产物，不是源码。** grep 会同时命中它和真正的 `agent/<模块>/`，改错地方不会有任何报错。

## 一个文件里看不到的流程

- **一次 headless 调用的两条流**：stdout 只在跑完时写一个 JSON 对象（原子，不流式）；进度、开销、错误走 stderr，一行一个 JSON、按顶层 key 分辨种类。读取方必须**两条管道都无条件排水** - 只读一条会让另一条填满后把子进程卡死。`agent/pzt_client.py` 为此起两个读取线程，不能退回 `communicate()`（它不流式，进度会全变事后回放）。
- **锦标赛只有一个入口。** `core::tournament::cluster_and_choose` 一次调用里做完分簇、场次推进、判定胜者。dedup 与 curate 共用它，区别只在传进去的 `CompareFn`：AI 路径调模型，`cli/compare` 的人工路径等一次按键。想改选择逻辑时找这里，不要去 dedup 或 curate 里找。
- **批量作用域只有一份解析。** `*` / `#标签` / `.` 四支写法与"默认排除废片/重复"策略都在 `core/scope`，交互与 headless 共用。cli 侧**只有** `browse.cpp` 的 `resolve_scope_with_view` 一处把当前视图交给 core - 新增带作用域的命令时接到那里，不要自己取视图。
- **系统标签有两个身份。** 库里永远存 canonical 中文名（`废片`/`重复`）；**显示名**跟界面语言走，归 `cli/i18n`；**标识符解析**（用户打 `#Reject`）不随语言变，归 `core/scope`。别名的唯一定义在 `core/tagging`，`pzt images --json` 的 `system_tags` 字段是第二个消费者 - agent 必须按它判断废片/重复，拿中文存储名去比对显示用的 `tags` 会在改名时**静默地不过滤任何东西**。

## 陷阱

- **改 `initialize_schema` 必须同时 bump `core/db/schema.h` 的 `kSchemaVersion` 并写对应的 `migrate_vN_to_vN+1`**，加一列也不例外。忘了不会报错：已盖章的库走快路径根本不执行那段代码，于是新列在**所有存量安装**上永远不出现，而开发机新建的库一切正常。这是本仓库最难发现的一类 bug，已经真实丢过一次用户数据（详见 `docs/RELEASE.md`）。
- **`CHANGELOG.md` 由 `git-cliff` 生成，绝不手改。**
- **`projects.archived_at` 是死列，故意留着。** 归档功能已删，列没删（理由写在 `core/db/schema.cpp`）。看到它不要以为有归档态。
- **`core/decode/decode.cpp` 写死 `CreateImageAtIndex(src, 0)`**，正确 API 是 `CGImageSourceGetPrimaryImageIndex()`。今天不出错（辅助图不进图像列表），HEIC 图像序列这类多 item 容器会让它偏。
- **同一条扩展名白名单有两份**：`core/project/project.cpp` 与 `agent/transport/watchfolder.py`。加格式时两处都要改。
- **`cli/commands/` 的 `browse.cpp` 与 `commands.cpp` 是零测试覆盖区**（cli 测试只编 i18n / kitty / text），而 `commands.cpp` 是 headless 命令面的全部实现。跨进程契约的漂移没有自动化检查在守。
- **worktree 里必须先建一次 release 构建物**，哪怕这次不碰 C++ - 否则 `agent/pzt_client.py::default_pzt_bin()` 会**静默**回落到 brew 装的旧 `pzt`，症状是一句看不出所以然的 SQL 错误。`agent/tests/test_pzt_client.py::test_default_pzt_bin_points_at_repo_build_release_cli_pzt` 是这件事的哨兵，它红了不是环境噪音。命令见 `AGENTS.md`。
