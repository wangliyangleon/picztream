# PZT Fix-it Night 全面评审报告(2026-07)

基于 commit `8f5af14`(M3 增量一 + 近似重复检测已完成)对全部文档(Roadmap、M0-M3 PRD/Eng Design、RAW_Support、Optimization_Backlog)与全部 `core/`、`cli/` 源码的一次完整评审。分四个视角(产品经理/用户/架构师/工程师)展开叙述性分析,所有可执行条目统一编号 F-01 到 F-48,集中在"逐项清单"一节,文末附汇总矩阵与建议执行顺序。

本报告只记录问题与提案,不改任何代码。已有的 `docs/Optimization_Backlog.md` 里仍然有效的条目(连接复用观察、交互菜单无自动化测试)在这里被重新评估并交叉引用,不重复维护两份。

## 评级与分类定义

**难度**(工程量):
* S: 半天以内
* M: 一到三天
* L: 三天以上,或需要先出设计文档

**复杂度**(风险与影响面):
* 低: 局部改动,行为清晰,单测/真机可直接覆盖
* 中: 跨模块,或有行为语义变化需要拍板
* 高: 影响核心路径或数据,需要谨慎设计与回归

**优先级**:
* P0: 已交付功能实际不可用,或与已验收 PRD 的用户故事直接矛盾,应立即修
* P1: 明显影响日常体验、正确性、成本,近期修
* P2: 值得做,正常排期
* P3: 记录在案,等数据或真实需求触发

**类别**(每条标注,可多个):
* `Bug`: 行为与预期/文档不符
* `健壮性`: 崩溃、异常边界、并发安全
* `性能/内存`
* `需求proposal`: 新功能或行为扩展提案,需要产品拍板
* `交互打磨`: 不改能力,只改顺畅度
* `纯重构`: 行为完全不变的结构调整
* `纯清理`: 死代码、冗余数据、过期残留
* `工程规范`: C++ practice、防御性、一致性
* `文档修正`
* `观察记录`: 暂不行动,只留档

**来源视角**: 产品 / 用户 / 架构 / 工程,标注该条目主要由哪个视角发现(可多个)。

---

## 一、产品经理视角

### 1.1 核心用户与故事

PZT 的核心用户画像非常清晰,而且只有一个(M0 PRD 原话:"仅一个用户,即项目开发者本人"):**终端原生的摄影工程师**。他用 LazyVim 写代码,用徕卡 Q3(60MP DNG)和富士 X-T5(RAF)拍照,每次旅行回来几百上千张照片。他不缺修图工具,缺的是把"筛选"这个最高频、最枯燥的动作压缩到极致的工具。他对延迟的感知阈值是键盘手速级别的,任何一次"等一下"都会打断心流。

这个画像决定了产品的三条铁律,后面所有断点判断都以它们为尺:

1. **零延迟心流**: 从按键到画面 100ms 内,任何操作不弹窗、不打断
2. **判断权在人**: AI/算法只给建议(写库、打标签),永远可撤销、可覆盖
3. **不碰用户数据**: 原始文件永不修改,一切状态都在 PZT 自己的库里

理想故事(用来对照走查的"黄金流程"):

> 周日晚上,李昂结束了三天的京都之行,存储卡里 1400 张照片(600 张 Q3 DNG + 800 张 X-T5 JPEG)。他打开 Ghostty:`pzt new kyoto ~/Pictures/kyoto2026 --support-raw`,进度条走完 RAW 预览缓存,按任意键直接进入浏览。左手 h/l 秒切,右手 x 干掉糊片,space 给家人照打上 Family。翻到一半,他敲 `:` 输入 `/ai_eval *`,让 AI 在后台把曝光/构图/对焦评估跑上,自己继续翻,不用等。手动过完一遍后 `/dedup #Family` 收敛连拍,按 g 筛出重复组逐一复核、改判几张。最后 r 给九宫格候选套上 Warm,g e 导出到 ~/Desktop/kyoto_moments。全程没碰鼠标,一个小时,从落座到发圈。

### 1.2 黄金流程走查: 断点清单

按流程顺序,每个断点指向逐项清单里的编号:

| 流程阶段 | 断点 | 条目 |
|---|---|---|
| 导入 | macOS TCC 保护目录(~/Pictures 下常见)会让 `pzt new` 直接崩溃,故事第一步就可能翻车 | F-06 |
| 导入 | 纯 JPEG 大项目逐张读 EXIF 要数秒,全程无输出,像卡死 | F-47 |
| 浏览 | 关掉重开永远回到第一张,多次分批选片(真实习惯)每次都要重新定位 | F-24 |
| 浏览 | 不知道"还剩多少张没标",j/k 工作流没有进度感 | F-27 |
| AI 评估 | 全分辨率原图直接上传,成本膨胀数倍,大文件可能直接超 API 限制 | F-02 |
| AI 评估 | 没有 key/断网时提交后杳无音信,没有任何失败提示 | F-03 |
| AI 评估 | 已打废片的照片也会被批量评估,白花钱 | F-26 |
| AI 评估 | **评完之后结果不可行动**: 无法按"不达标"筛选/排序,AI 数据只是单图装饰 | F-09 |
| 去重 | **跑完之后无法在应用内查看重复组、无法手动改判**,PRD 用户故事直接断裂 | F-01 |
| 去重 | 阈值写死、无 EXIF 时间的图被静默跳过,用户"预期的重复没被干掉"却无从排查 | F-08 |
| 去重 | 结果只报数字,没有"跳去看"的入口 | F-11 |
| 去重 | 冻结几十秒期间按的键会在解冻后突然回放执行 | F-25 |
| 导出 | 每次都要重新敲一遍目标路径 | F-43 |

### 1.3 与产品内核冲突的点

这些不是简单的 bug,是方向性的张力,需要产品层面持续盯着:

1. **`/dedup` 阻塞冻结 vs 零延迟心流**。M3 Dedup PRD 明确接受了这个代价("刻意的简化"),这个决定本身没问题,但复评条件应该明确写下来: 一旦真实项目跑一次超过约 15 秒、或者用户开始习惯性在等待时切走,就应该启动异步版本立项。F-13(有界解码)能把这个冻结时间压缩数倍,可以先于异步化做。
2. **云端 AI 调用 vs "零网络依赖/本地优先"基调**。Roadmap 的技术基调和 M3 的云端实现之间的张力 PRD 已经记录未决。当前增量(手动触发)可以接受,但"自动介入"增量启动之前必须先解决: 成本护栏(F-02 是第一步)、失败可见性(F-03)、以及供应商可配置(F-10)。
3. **60MP 秒切承诺 vs 全尺寸解码架构**。M0 spike 早就测出 60MP 档解码+传输约 330ms 超预算,当时的结论是"降采样兜底是必须的真实路径",但现在的降采样发生在全量解码之后,解码本身(约 3ms/MP)就已经超支。用户手里的 Q3 恰好是 60MP(RAW 路径靠半分辨率缓存侥幸躲过,纯 60MP JPEG 躲不过)。F-13 是兑现这个承诺的正路。
4. **"评估"与"选片"之间缺一环**。产品定位是选片辅助,但评估结果目前止步于展示。用户拿到 1400 张的评估结果后,产品没有回答"然后呢"。F-09 是把 AI 能力真正闭环进选片工作流的关键一步,建议作为 M3 下一个增量的主体。

---

## 二、用户视角

### 2.1 场景模拟

**场景 A: 手机图/微信图混合项目(无 EXIF 时间)**。用户把一批微信保存的图和截图建成项目想去重: `/dedup *` 报 "0 组"。实际原因是这些图没有 EXIF 拍摄时间,`load_metas` 里 `captured_at IS NOT NULL` 把它们全部静默排除了(core/dedup/dedup.cpp:78),而结果文案完全不提。用户的结论只会是"这功能不好使"。(F-08)

**场景 B: 大型旅行项目重度使用**。3000 张的项目里 `/ai_eval *`: 提交前逐张 `get_image`,每张开一条新 SQLite 连接并重跑全套 schema 初始化,按下回车后界面冻结数秒才出现"已提交"(F-07)。`/dedup *` 同理,而且在 y/N 确认弹出来之前就先冻结一轮。冻结期间习惯性按的 h/l/x 会在解冻后一口气回放,可能误打标签(F-25)。

**场景 C: 复核 dedup 结果(当前完全走不通)**。用户想"g 筛出重复标签逐组看,选错了手动改":
* `g` 菜单: `tags_for_menu` 过滤掉全部系统标签(cli/menu/tag_menu.cpp:158-166),菜单里只有 `0:废片` + 用户标签 1-9,"重复"根本不在列表里,**筛不了**;
* `space -` 摘除菜单用同一份列表,**摘不掉**;`space` 添加菜单同理,**补不上**。
* 唯一的出路是退出去 `pzt export kyoto 重复 /tmp/dups` 把重复图导出来看,或者用 `/dedup #重复` 这种奇技淫巧。M3 Dedup PRD 的用户故事("用 g 筛选出打了 duplicate 标签的照片,一组一组看过去"/"手动摘掉 duplicate 标签")在当前实现里是断的。(F-01,本报告唯一的 P0)

**场景 D: AI 成本敏感用户**。开着 `/ai_eval *` 跑了 500 张后看账单: 每张都是全分辨率 JPEG(单张请求体可到 10MB+ base64),token 消耗是合理尺寸的许多倍(F-02);其中几十张早就打了废片,也照评不误(F-26);有两张因为网络抖动失败了,他永远不会知道,信息栏永远显示"未评估"(F-03)。

**场景 E: 换终端窗口大小**。浏览中把 Ghostty 窗口拖大: 下一次按键前画面不响应,按键后新边框画在新位置,旧边框残影留在原地,图片位置错乱,直到再切一张图才勉强恢复。(F-20)

### 2.2 可能的新需求(需求 proposal 汇总)

* 按评估结果筛选/排序/批量处理(F-09,最重要的一个)
* 配置文件: 键位、AI 供应商、dedup 参数、预取窗口、界面宽度(F-12,M0 PRD 的键位自定义承诺至今未兑现)
* 会话续点: 重开项目回到上次位置(F-24)
* 对当前筛选视图批量打标签(F-44)
* 导出路径记忆/默认值(F-43)
* unarchive(F-29,M0 起就挂着的已知缺口)

### 2.3 低频/存疑功能盘点

| 功能 | 现状判断 | 建议 |
|---|---|---|
| `pzt tag list` | 只读列表,与应用内 space 菜单信息重叠 | 保留(维护成本趋近于零),不投入 |
| `pzt recipe rename/delete` 的 `preset:N` 寻址 | 语法冷僻,rename 甚至没有应用内等价物 | 记录不一致(F-30),等真用到再说 |
| `pzt archive` | 没有 unarchive,归档基本是单程票 | F-29 |
| `r v` 原图/风格化切换 | 低频但便宜,有存在价值 | 保留 |
| `--no-prune` | 外置硬盘场景的保险丝 | 保留 |
| Claude 供应商 | 代码在,但运行时不可达(写死 Gemini),等效死功能 | F-10 |
| 有序标签(is_ordered/position) | 存了顺序但没有任何调序 UI,九宫格故事只兑现了一半 | F-28,先拍板要不要做 |

---

## 三、架构师视角

### 3.1 值得肯定的底子(不需要动的部分)

* core/cli 分层纪律执行得很好: core 无终端依赖,渲染探测/passthrough 全部只在 cli;为 M4 headless 层预留的复用性是真实的。
* 依赖注入模式全线一致(`DecodeFn`/`RawDecodeFn`/`HttpPostFn`/`PreviewDecodeFn`/`EvaluationFn`),core 的单测覆盖因此扎实(192 例)。
* RAII 纪律好: `CbreakMode`/`AltScreen`/`Stmt`/`DebugLogRedirect`,jthread 成员声明位置保证析构顺序,注释把理由写清了。
* 文档先行的流程(PRD -> Eng Design -> 增量)在代码注释里可追溯,历史决策不失忆。
* 延迟日志文化(key-to-render、prefetch hit/miss)符合工程契约。

以下问题都是在这个健康底子上的修缮,不是推倒重来。

### 3.2 核心架构问题

**A. core 内部出现了对门面的反向依赖,且"按扩展名分发预览解码"这份逻辑已复制三份**(F-16)。`core/ai/evaluation_worker.cpp` include 了 `core/api.h`(下层模块依赖门面,而 api.h 又 include evaluation_worker.h,靠 .cpp 才没成环);`core/dedup/dedup.cpp` 为了避免同样的问题选择了复制一份 `has_raw_extension` + `default_decode_preview`;`core/api.cpp` 自己还有一份。加上 `resolve_path`(缓存优先取路径)在 evaluation_worker 和 dedup 各一份。这不只是洁癖问题: **今后加 CR3/NEF 支持时,`project.cpp` 的 `kRawExtensions` 认了新扩展名,而另外两份写死 `.dng/.raf` 的判断不认**,新格式会被扫描进项目、却在预览分发时走错路径。RAW_Support.md 说"留好了口子"只对扫描那一份成立。方案: 抽一个极小的 `core/media`(或挂在 core/decode 旁): `is_raw_path()` / `decode_preview_file()` / `resolve_preview_path()`,api.cpp 变转调,ai/dedup 直接用。纯重构,行为不变。

**B. SQLite 并发姿态没跟上多线程现实**(F-04)。M0 时代"每次调用开一条连接"是单线程串行的,无害;M3 之后 EvaluationWorker 在后台线程写 `image_evaluations`,主线程同时可能写 `image_tags`,两条连接、默认 journal、**没有 busy_timeout**(core/db/database.cpp 全文无此调用): 写锁窗口撞上就是 SQLITE_BUSY,`Stmt`/`exec` 直接 throw。在 worker 线程里逃逸异常等于 `std::terminate`。一行 `sqlite3_busy_timeout(db, 2000)` 就能消掉绝大部分风险,顺带评估 WAL(读写不互斥,副作用只是 config 目录多 -wal/-shm 文件)。

**C. 异常没有边界**(F-05)。core 用 `Result<T,E>` 表达业务错误,把真正的异常留给"不该发生"的场景,这个约定本身对。但 cli 侧没有任何一层 catch: uncaught 异常在 libc++ 上**不保证栈回退**,`AltScreen`/`CbreakMode` 的析构不会跑,终端直接留在无回显、备用屏的状态,这是最伤"工具可信度"的一类事故。修法极便宜: `main()` 的 dispatch 包一层 try/catch(有 handler 就保证 unwinding,RAII 全部生效),catch 里打一行错误。配合 B 一起,"评估落库撞上打标签"这类小概率事件从"终端报废"降级成"一条错误提示"。

**D. 门面"每调用一连接"的成本结构变了,需要重新审视**(F-07 热点修复 + F-38 观察)。Optimization_Backlog 里"观察-暂不处理"的结论基于 M1 时代的测量(60ms 无感)。此后 schema 初始化显著变厚(7 张表 DDL + 6 次 ensure_column 各带 PRAGMA table_info + 4 次 ensure_column_dropped 也各带一次,合计每次开库约 20 条语句),而且出现了新的调用形态: **批量循环里逐张走门面**(`/ai_eval *`、`/dedup` 前置统计),这是 N × (开库 + 全套 DDL) 的乘法。建议分两层处理: 热点用批量查询直接消灭(F-07,一条 `SELECT image_id FROM image_evaluations WHERE image_id IN (...)`);"每帧 4 次连接"的常态路径先重测一次数字再决定(F-38),若 M4 headless 层立项,顺势引入一个持有连接的 `core::Session` 是最自然的时机,现在不预做。

**E. 配置层缺位造成的散落硬编码**(F-12)。供应商(browse.cpp 里两处 `Provider::Gemini`)、dedup 阈值(`find_and_tag_duplicates` 里写死 10s/5bit,把 `find_duplicates` 的默认参数架空)、预取窗口(3)、界面宽度(0.7)、Esc 消歧超时(20ms)。建议 cli 层加 `~/.config/pzt/config.json`(nlohmann 已是依赖),core 保持无配置感知(全部经函数参数传入,现有参数面已经支持)。分层不破,散点收拢。

**F. 控制台正在长成一个子系统,但它的纯函数埋在 browse.cpp 匿名命名空间里不可测试**(F-22)。`take_scope_token`(带引号解析,edge case 最多的一段)/`split_console_command`/`wrap_tokens` 都是确定性纯函数,却因为所在位置没有任何单测。`/` 命令还会继续加(F-09 的 `/filter` 等),建议此时抽 `cli/console`(或并入 cli/text)小库并补测试。顺带记录: browse.cpp 已从拆分时的 648 行涨到 1138 行,cmd_open 约 700 行,暂不强拆,但下一个大特性落进去之前应该先把帧渲染段抽出函数。

### 3.3 冗余与清理清单(纯清理类)

* `DedupSummary::unevaluated_image_count`: 唯一调用方(handle_dedup_command)为了 y/N 提示自己数了一遍,门面返回的这个字段无人使用,而它的统计又是一轮 N 次 `get_image`。删字段、删统计循环、改两处测试。(并入 F-07)
* `core::find_image_by_path` 门面(api.h:116): 服务对象是 6.4.7 已退休的调试命令,cli 无任何调用方。(F-31)
* `ensure_default_presets` 在每次 `list_presets`/`list_versions` 门面调用时重算 17³ Warm LUT(约 1.5 万次 sin + 60KB 分配)再被 INSERT OR IGNORE 扔掉,按 r 键一次就跑一遍。播种挪到建库时,或 seed 前先 SELECT。(F-32)
* `ensure_column_dropped` 四连(schema.cpp:210-213): 只为清理 M3 开发期的中间态列,用户的真实库早已迁移完;每次开库都白跑 4 次 PRAGMA table_info。(F-33)

---

## 四、工程师视角

按"正确性 > 健壮性 > 性能 > 规范"的顺序。每条的完整描述在逐项清单,这里给定位和证据。

### 4.1 正确性(Bug)

* **F-01** 系统标签在三个菜单(space/space-/g)全部不可达,只有废片有硬编码 0 号位;"重复"标签查看/增删断链。cli/menu/tag_menu.cpp:158-166、cli/i18n/i18n.cpp:1224-1234 与 1329-1335 可证。
* **F-02** `core/ai/evaluation_worker.cpp:98` 解码后未经任何缩放直接进 `request_json`,`core/ai/ai.cpp:268` 原尺寸编码 base64。Claude API 单图 5MB 上限大概率直接 HttpError(这也解释了为何该路径"没验证过"),Gemini 能吞但成本/延迟数倍膨胀。
* **F-08** `core/dedup/dedup.cpp:256-258` 把 `time_window_seconds=10, hash_threshold=5` 写死,头文件里的默认参数形同虚设;`load_metas`(dedup.cpp:78)静默丢弃无拍摄时间的图。这两条合起来正对着用户上一轮遗留的"预期的重复没被干掉"未决问题。
* **F-18** `core/dedup/dedup.cpp:264` 忽略 `add_tag` 返回值,`tagged_count` 无条件自增: 若用户手建过带 cap 的同名"重复"标签(ensure_duplicate_tag 会直接复用,文档已注明),超 cap 的图实际没打上标签,汇总却照报。
* **F-41** `pzt list` 未展示各标签计数,M0 PRD 功能需求原文有此承诺("包括根路径、图片总数和各标签下的图片数量"),实现(i18n.cpp:334-346)从未包含。属文档/实现漂移,二选一收口。

### 4.2 健壮性(崩溃/并发/终端状态)

* **F-06** `core/project/project.cpp:104` `fs::recursive_directory_iterator(root)` 用的是抛异常重载: 目录不存在、或递归撞上 macOS TCC 保护子目录(EPERM)直接抛,`pzt new`/`pzt rescan` 整个进程 abort。另: `cmd_new`(commands.cpp:97-107)把不认识的 `--` 参数当 folder_path,`pzt new x --supportraw`(拼错)就会触发上面的崩溃。cmd_rescan 有未知参数拒绝逻辑,cmd_new 没有,不对称。
* **F-04** 全仓库无 `sqlite3_busy_timeout`/WAL(已 grep 确认)。后台评估落库与主线程写标签并发时 SQLITE_BUSY 即抛异常;发生在 worker 线程 = `std::terminate`。
* **F-05** `cli/main.cpp` 无 try/catch。主线程 uncaught 异常(上面的 BUSY、磁盘满、库损坏)不保证 unwinding,终端留在 cbreak+AltScreen。
* **F-25** `/dedup` 冻结期间的按键留在 tty 缓冲区,解冻后连续回放(可能连按出 x/q)。Dedup PRD 风险清单原文就要求"实现阶段需要真机验证并记录",实际没有收口。修法: 阻塞操作返回后 `tcflush(STDIN_FILENO, TCIFLUSH)`。
* **F-20** 无 SIGWINCH 处理且从不整屏清除: 终端 resize 后旧边框/图片残影错位,直到下一次按键也只是部分修复。
* **F-21** `perform_curl_post`(ai.cpp:218-255)未设 `CURLOPT_NOSIGNAL`(多线程 + 信号驱动 DNS 超时是经典崩溃源)、无连接超时(只有 60s 总超时)。
* **F-03** 评估失败只写 stderr,而 stderr 默认被 DebugLogRedirect 吞进 /dev/null: 无 key/断网时用户视角就是"提交了,然后永远没有然后"。

### 4.3 性能/内存

* **F-07** `/ai_eval *`、`/dedup` 的 N+1 门面调用: 每张图一次 `core::get_image` = 开连接 + 约 20 条 schema 语句 + LEFT JOIN + 关连接;dedup 还统计两遍(cli 一遍、`find_and_tag_duplicates` 内部再一遍且结果无人用)。3000 张级项目按键即冻结数秒。M3 Eng Design 的"性能提醒"预告成真,批量查询方案文档里已经写好了。
* **F-13** 预取缓存持有全分辨率 RGBA: 24MP 一张 96MB,窗口 7 张约 0.7GB;60MP JPEG 解码本身(约 3ms/MP)就超 100ms 预算(M0 spike 数据)。统一解法是有界解码: `CGImageSourceCreateThumbnailAtIndex` + `kCGImageSourceThumbnailMaxPixelSize`(JPEG 原生支持 DCT 域 1/2/4/8 降尺度,解码耗时随目标像素走),同时惠及 dedup 哈希(只要 9x8)和 AI 上传(F-02)。
* **F-14** `PrefetchCache::get`(prefetch.cpp:150)按值返回整块像素,几十 MB 拷贝且发生在持锁期间(顺带阻塞 worker)。Entry 存 `shared_ptr<const DecodedImage>` 即可。
* **F-15** `set_current`(prefetch.cpp:98-105)每次按键为**全项目**构建 id->路径 map(O(n) 字符串分配),包括不导航的按键;且这段开销在 key-to-render 计时窗口之外,延迟日志量不到它。只解析窗口内的 id 即可。
* **F-36**(杂项包内)key-to-render 计时起点在循环顶部,漏掉了按键处理段(菜单/DB 写)和 set_current 本身;p95 取样索引 `size*0.95` 在小样本时取到 max。

### 4.4 工程规范(C++ practice)

* **F-17** 写路径 `sqlite3_step` 返回值不查: recipe.cpp 四处(create_version:163 失败时还会把上一条 insert 的 rowid 当新 id 返回、rename:178、delete:191、set_image_recipe:215)、evaluation_worker.cpp:169(落库失败静默,generation 照样 +1 触发一次空重绘)、project.cpp 的 backfill/缓存路径回写。project/tagging 模块是查了就 throw 的,同仓库两套标准。
* **F-19** `Result<T,E>` 只有 assert 防线: NDEBUG(RelWithDebInfo,用户日常构建)下 `Result<void,E>::error()` 解引用空 optional 是 UB;value()/error() 无 `[[nodiscard]]`,而 F-18 就是一个真实的"返回值被无声丢弃"案例。对齐 Abseil StatusOr 惯例: 类型与工厂加 `[[nodiscard]]`,取值函数在 NDEBUG 下也 fail-fast。
* **F-34** `core/raw/raw.cpp:102` 把 RGBA 第 4 字节填 0: 当前所有消费方都忽略 alpha,但 kitty 渲染用的是 f=32(RGBA,alpha 有效),CG 解码路径实测填的是 255;哪天有人把 `decode_full` 输出直连渲染就是一张全透明图。防御性改 255。
* **F-36** 杂项包: `write_stdout` 忽略短写;`usleep` 换 `std::this_thread::sleep_for`;上面提到的计时起点与 p95;`compute_dhash` resize 失败返回 0 哈希(两张失败图会互判重复,现路径不可达,改 optional 更稳)。
* **F-35** `is_wide_codepoint` 未覆盖 emoji 区(0x1F300 起): 文件名带 emoji 时列宽算错、边框错位。
* **F-40** dedup 的 IN 子句逐个绑定变量,SQLITE_MAX_VARIABLE_NUMBER(本机 3.43.2 为 32766)之上会 prepare 失败抛异常;与 F-07 的批量查询一起按 500 分块即可。
* **F-39** `groups_by_root` 是 unordered_map,分组输出顺序跨运行不稳定: 打标签结果集合一致,但违反 Dedup PRD "确定性" NFR 的字面;输出前排序即可。

---

## 五、逐项清单

### P0

---

**F-01 "重复"系统标签在交互菜单不可达,dedup 复核/人工改判工作流断裂**
类别: Bug | 来源视角: 产品 + 用户 + 工程

背景: `tags_for_menu`(cli/menu/tag_menu.cpp:158-166)过滤全部 `is_system` 标签,space(加)/space-(摘)/g(筛选) 三个菜单共用它,菜单里只有硬编码的 `0:废片` 与用户标签 1-9。M3 引入第二个系统标签"重复"后,它在应用内: 筛不了、摘不掉、也补不上。M3_Dedup_PRD 的两条用户故事("g 筛出 duplicate 逐组复核"、"手动摘掉/换一张保留")因此在实现层面断裂。控制台 `/ai_eval #重复`、`pzt export <项目> 重复 <目录>` 倒是能按名字寻址,但没有任何应用内查看路径。

**已确定方案**(2026-07-11 拍板,方案 (a)): 数字 `9` 固定分配给"重复",用户动态标签让出 9 号、上限从 9 改成 8。

* `cli/menu/tag_menu.cpp` 的 `tags_for_menu()` 截断条件从 `size() > 9` 改成 `size() > 8`。
* `handle_space_key`、`handle_remove_tag_submenu`、`handle_g_key_prompt` 三处新增一个 `std::optional<TagId> duplicate_tag_id` 参数(跟现有 `reject_tag_id` 并列),`c == '9' && duplicate_tag_id` 分支跟现有 `c == '0'`(废片)对称处理;菜单文案行(`tag_menu_options_line`/`filter_menu_options_line`)只在 `duplicate_tag_id` 有值时才追加 `9:重复`。
* `duplicate_tag_id` 在 `cmd_open` 里用 `find_tag_by_name(project_id, kDuplicateTagName)` 取(**不是** `ensure_duplicate_tag`——打开菜单不该有创建标签的副作用,没跑过 `/dedup` 的项目不该看到 9 号位)。

未采纳: (b) 系统标签追加在用户标签之后动态编号(编号会漂移,违背"数字键肌肉记忆固定"的设计原则);(c) 控制台加 `/filter #标签名`(和 g 菜单形成两套筛选入口,交互不统一——这个思路后来被 F-09 的 `/filter` 二级筛选设计吸收,但语法和定位不一样,见 F-09)。

难度 S-M | 复杂度 低 | 优先级 **P0**

### P1

---

**F-02 AI 评估把全分辨率图片直接编码上传**
类别: Bug + 性能/内存 | 来源视角: 工程 + 用户

背景: `process_request`(core/ai/evaluation_worker.cpp:98)拿到 `decode_preview_file` 的结果后没有任何缩放,`request_json`(core/ai/ai.cpp:268)按 q=0.9 原尺寸编码再 base64(+33%)。纯 JPEG 项目里这是全分辨率(24MP 照片单请求体可达 10MB+): Claude API 单图 5MB 上限大概率直接 HttpError(Claude 路径"从未真实调通"的一个可能根因),Gemini 能吞但 token 成本与延迟成倍膨胀,评估质量并不会因此更高(视觉模型输入端本来就会被降采样)。

方案: 发送前 `resize_rgba` 到长边约 1024-1568px(Claude 视觉最优区间),质量可顺带降到 0.8。改动点在 evaluation_worker(缩放后再交给 evaluation_fn)或 request_evaluation 内部,推荐前者(通用层保持不知情)。F-13 落地后可换用有界解码进一步省掉全量解码本身。

难度 S | 复杂度 低 | 优先级 P1

---

**F-03 AI 评估失败对用户完全不可见**
类别: Bug + 交互打磨 | 来源视角: 用户 + 工程

背景: 失败(MissingApiKey/网络/解析)只打 stderr,而 stderr 默认被 DebugLogRedirect 重定向进 /dev/null;`generation_` 无论成败都 +1,poll 触发的重绘里什么都不会变。用户提交后既等不到结果也看不到原因,`/tasks` 显示队列空,像什么都没发生过。

方案: EvaluationWorker 增加线程安全的"最近一次失败"查询(如 `std::optional<std::pair<ImageId, EvaluationError>> take_last_failure()`),browse 的 poll 分支在 consume_new_result 后顺带取一次,非空则 status_override 一句"图 X 评估失败: 网络错误"。i18n 补对应文案。

难度 S | 复杂度 低 | 优先级 P1

---

**F-04 SQLite 未设 busy_timeout,双连接并发写可抛异常直至 terminate**
类别: 健壮性 | 来源视角: 工程 + 架构

背景: 见 3.2-B。EvaluationWorker 后台线程写 `image_evaluations` 与主线程写 `image_tags`/`images` 是两条连接;默认 busy handler 为空,写锁冲突立刻 SQLITE_BUSY,`Stmt` 构造或 `exec_simple` throw。worker 线程里逃逸异常 = `std::terminate`;主线程则触发 F-05。跨进程场景(一边 `pzt open` 一边另一个终端 `pzt rescan`)同样适用。

方案: `Database::open_at` 里 `sqlite3_busy_timeout(db, 2000)`;评估 `PRAGMA journal_mode=WAL`(读写不互斥,进一步压缩冲突窗口,代价是 config 目录多 -wal/-shm 两个文件)。

难度 S | 复杂度 低 | 优先级 P1

---

**F-05 cli 无异常边界: uncaught 异常不保证栈回退,终端留在 cbreak+AltScreen**
类别: 健壮性 | 来源视角: 工程 + 架构

背景: 见 3.2-C。`main()` 直接 dispatch,cmd_open 内任何 core 异常(F-04 的 BUSY、磁盘满、库损坏)uncaught;libc++ 对 uncaught 异常直接 terminate 不回退,`AltScreen`/`CbreakMode` 析构不执行,用户终端变成无回显状态。

方案: main() 的 dispatch 包 try/catch(存在 handler 即保证 unwinding,所有 RAII 生效),catch 打一行 "pzt: 内部错误: what()" 后返回非零。10 行改动。

难度 S | 复杂度 低 | 优先级 P1

---

**F-06 扫描目录不存在/无权限时 `pzt new`/`pzt rescan` 崩溃;cmd_new 不拒绝未知参数**
类别: 健壮性 + Bug | 来源视角: 工程 + 用户

背景: 见 4.2。`recursive_directory_iterator` 抛异常重载 + macOS TCC 保护目录是高概率真实场景(~/Pictures 下就有);`pzt new x /打错的路径` 或拼错的 flag(被当作 folder_path)都直接 abort。

方案: 迭代器改用 `fs::directory_options::skip_permission_denied` + error_code 重载;folder 不存在时干净返回 `NoImagesFound`(或加 `FolderNotFound` 错误值,i18n 一条新文案);cmd_new 对未知 `--` 开头参数报错退出,对齐 cmd_rescan。

难度 S | 复杂度 低 | 优先级 P1

---

**F-07 批量控制台命令 N+1 门面调用,大项目按键即冻结;dedup 统计两遍且返回值字段无人用**
类别: 性能/内存 + 纯清理 | 来源视角: 工程 + 架构

背景: 见 3.2-D 与 4.3。三处热点: `handle_ai_eval_command`、`handle_dedup_command` 的未评估统计、`find_and_tag_duplicates` 内部的重复统计(dedup.cpp:241-245,产出的 `unevaluated_image_count` 唯一调用方并不使用,纯冗余)。`pick_keep_id` 逐成员 `get_image` 是同连接、组规模小,可顺手不必须。

方案: (1) 门面加批量查询 `std::vector<ImageId> evaluated_subset(const std::vector<ImageId>&)`(一条 `SELECT image_id FROM image_evaluations WHERE image_id IN (...)`,按 500 分块防变量上限,顺带解决 F-40);cli 两处循环改为对差集提交。(2) 删除 `DedupSummary::unevaluated_image_count` 字段与统计循环,调整两处测试。

难度 S-M | 复杂度 低 | 优先级 P1

---

**F-08 dedup 阈值写死、无拍摄时间的图静默跳过、调参无观测手段**
类别: Bug + 需求proposal | 来源视角: 用户 + 产品 + 工程

背景: 见 4.1。三件事叠加,正对用户遗留的"预期重复没被干掉"未决问题: (1) `find_and_tag_duplicates` 写死 10s/5bit,头文件的默认参数不可达;(2) `captured_at IS NULL` 的图(微信图/截图/编辑过的导出件)整体被排除且结果文案不提;(3) 分组不达预期时用户没有任何观测手段判断是"时间窗切开了"还是"哈希阈值太紧"。

**已确定方案**(2026-07-11 拍板): 参数只走 F-12 的 `Settings`(`dedup_time_window_seconds`/`dedup_hash_threshold`),`/dedup` 命令本身不接受内联参数覆盖(想调参就改配置文件,控制台命令保持简单)。另外两点原样采纳,都要做:

* `DedupSummary` 新增 `skipped_no_capture_time` 字段,`find_duplicates`/`find_and_tag_duplicates` 统计范围内 `captured_at IS NULL` 被跳过的图片数,结果文案(`msg_dedup_result`)带出来。
* `--debug` 模式下,`detail::find_duplicates_impl` 每完成一次候选簇内两两汉明距离比较,往 stderr 打一行明细(image_id 对 + 距离),走现成的 debug 面板日志通道,不新增机制。

依赖 F-12(`Settings` 结构体要先有 `dedup_time_window_seconds`/`dedup_hash_threshold` 两个字段)。

难度 M | 复杂度 中 | 优先级 P1

---

**F-09 评估结果不可行动: 缺按"达标/分数"的筛选、排序与批量处理**
类别: 需求proposal | 来源视角: 产品 + 用户

背景: 见 1.3-4。批量评估完成后,数据只在单图信息栏展示;"把不达标的过一遍""按分从低到高看"这类最自然的下一步动作没有入口,AI 能力没有闭环进选片流。

**已确定方案**(2026-07-11 拍板,在原方案 (a) 基础上重新设计成"二级筛选"): 控制台 `/filter <条件>`,在**当前 g 筛选结果之上**(没有 g 筛选时就是全项目)再筛一层,不是加进 g 菜单——用户明确要求这是一个可以跟 g 标签筛选叠加的独立层,不是 g 菜单的第三种筛选项。条件词汇: `unevaluated`(未评估)、`fail`(评估不达标)、`reject`(废片)、`dup`(重复)。原方案 (b)(`/sort score`)、(c)(`/reject_failed`)这次不做,留待以后有真实使用手感再评估。

技术方案:

* `cmd_open` 内新增 `std::vector<ImageRef> g_filtered_images`(代表 g 层筛选结果——`g` 应用/清除时正常更新它,同时**总是**把二级筛选状态重置为"无"、把 `images` 重新指向 `g_filtered_images`。切 g 自动清空 `/filter`,已跟用户确认)。新增 `ConsoleFilterCriterion { Unevaluated, Fail, Reject, Dup }` + `std::optional<ConsoleFilterCriterion> active_console_filter`。
* `/filter <criterion>` 从 `g_filtered_images`(不是当前可能已经被筛过的 `images`,避免叠加筛两次)重新计算出 `images`;`/filter clear` 把 `images` 还原成 `g_filtered_images`、清空 `active_console_filter`。两种情况都用现有 `resolve_current_after_switch` 重新定位 `current_id`。
* 控制台分发返回类型从纯 `std::string` 升级成一个小结构体(`std::string status` + `enum class FilterAction { NoChange, Clear, Apply } action` + `ConsoleFilterCriterion criterion`)——`/filter` 要改主循环状态,不像 `ai_eval`/`dedup`/`tasks` 只是"发起动作、报个状态"。`handle_ai_console_command` 改造成返回这个结构体,其它分支只填 `status`,`action` 留默认 `NoChange`。
* 条件判定: `reject`/`dup` 复用 F-26 新增的 `core::tagging::images_with_tag`(一条查询,不是逐张查);`unevaluated`/`fail` 走逐张 `get_image()` 判断 `evaluation`/`passes_gate()`——这是已知的 N+1 模式,跟 `handle_ai_eval_command` 现有实现同一个量级,这次不顺带优化(那是 F-07 的范围)。
* 信息栏 index 行(`[N/Total]`)追加二级筛选标注,类似现有 `active_filter_tag_id` 那行的处理方式,新增一条 i18n 字符串。
* `g e` 导出快捷键不受影响——它按标签直接查库,不经过内存里的 `images`/`g_filtered_images`。

依赖 F-26 的 `images_with_tag`。改动面是这六项里最广的(控制台分发返回类型、`cmd_open` 状态机、信息栏展示都要动),建议单独一次会话完整实现+真机验证。

难度 M | 复杂度 中 | 优先级 P1

---

**F-10 AI 供应商硬编码 Gemini,Claude 路径运行时不可达**
类别: 需求proposal + 纯清理 | 来源视角: 产品 + 工程

背景: browse.cpp 两处写死 `Provider::Gemini`(M3_PRD 风险清单已有 TODO);`Provider::Claude` 分支成为运行时不可达代码,M3 验收标准"两家都能真实调通"实际无法验证。

方案: 最小版本读环境变量 `PZT_AI_PROVIDER=claude|gemini`(cli 启动解析一次,两个调用点用它);配置文件(F-12)落地后并入。顺带用真实 key 跑通一次 Claude 路径,把验收补上(注意 F-02 不修的话大图可能直接超限)。

难度 S | 复杂度 低 | 优先级 P1

---

**F-11 dedup 完成后没有"跳去复核"的动线**
类别: 交互打磨 | 来源视角: 产品 + 用户

背景: 跑完只报"X 组 Y 张",用户要自己想起"该去筛重复标签了"(而这一步现在还不可达,见 F-01)。

方案: F-01 落地后,结果文案带上入口提示("按 g 9 查看"),或直接追问一键 y 切入重复筛选视图。

难度 S | 复杂度 低 | 优先级 P1(依赖 F-01)

---

**F-12 配置文件机制缺失(键位自定义是 M0 PRD 的未兑现承诺)**
类别: 需求proposal | 来源视角: 产品 + 架构

背景: 见 3.2-E。散落的硬编码/环境变量: AI 供应商、dedup 阈值、预取窗口、界面宽度比例、语言(PZT_LANG)。M0 PRD"所有快捷键映射后续应支持在配置文件中自定义"从未落地。

**已确定方案**(2026-07-11 拍板,范围从"零散配置项"扩大成"统一全局设置系统"): 讨论中确认了两个关键前提——(1) 作用域先只做全局,不建项目级覆盖(dedup 阈值理论上有按项目调的场景,但目前没有实际需求驱动,真遇到了再加,不提前设计);(2) 这是"程序启动时读一次的静态配置",跟未来可能要做的运行时 `/setting` 控制台开关(F-26 讨论中提出,比如 `/setting eval_reject true` 立即生效)是两套不同定位的机制,可以并存、不互相阻塞——`/setting` 那部分不在这轮设计范围,见文末"未来任务"。

技术方案:

* 新模块 `core/settings/settings.h`/`.cpp`:
  ```cpp
  struct Settings {
    ai::Provider ai_provider = ai::Provider::Gemini;
    int dedup_time_window_seconds = 10;
    int dedup_hash_threshold = 5;
    bool eval_reject = false;
    bool dedup_reject = false;
    bool export_reject = false;
    bool export_dup = false;
    std::string lang = "zh";        // 原始字符串,core 不解释语义,cli::i18n 自己转换成 Lang
    double ui_width_ratio = 0.7;
    std::size_t prefetch_window = 3;
  };
  std::string default_config_path();          // 照抄 db::default_db_path() 的 XDG 解析方式
  Settings load(const std::string& path = default_config_path());
  ```
* 文件不存在 / JSON 解析失败 / 某个字段类型不对: **不抛异常、不报错**,缺失或非法的字段单独回退到默认值,其余合法字段照常生效(局部容错,不是整体回退)。
* 配置文件是纯手工编辑的 JSON(`~/.config/pzt/config.json`,扁平键,不嵌套),这一轮**不做** `pzt config` 命令去读写它。
* `core/api.h` 按现有 `using EvaluationWorker = ai::EvaluationWorker;` 的模式重导出 `Settings` 类型 + `load_settings()` facade。
* 消费点: `resolve_ai_provider()`(已存在,F-10)优先级改成 `PZT_AI_PROVIDER` 环境变量 > `settings.ai_provider` > 硬编码 Gemini;`find_and_tag_duplicates` 新增 `time_window_seconds`/`hash_threshold` 参数(默认值维持 10/5,`handle_dedup_command` 显式传 `settings.dedup_*`);`cli::i18n::init_lang()` 在现有 `PZT_LANG` -> 系统 `LANG` 链条中插入 `settings.lang` 作为中间优先级;`cmd_open` 的 `kWidthRatio` 常量和 `PrefetchCache` 的 `window=3` 改读 `settings.ui_width_ratio`/`prefetch_window`。

**未来任务(不在这轮范围,记录以免遗忘)**: 运行时 `/setting <key> <value>` 控制台命令,立即生效、不需要重启,典型用例是 `/setting eval_reject true`、`/setting dedup_reject true`。这个机制的具体语法、持久化方式(是否写回同一个 config.json、还是只在本次进程内存生效)留到真正要做的时候再设计。

难度 M | 复杂度 中 | 优先级 P1

### P2

---

**F-13 有界解码(subsampled decode): 内存、60MP 秒切、dedup 提速的统一杠杆**
类别: 性能/内存 | 来源视角: 工程 + 架构 + 产品

背景: 见 4.3 与 1.3-3。预取缓存存全分辨率 RGBA(24MP x 7 张约 0.7GB;60MP 单张解码约 180ms 直接超预算,M0 已知 gap);dedup 为了 9x8 哈希做全量解码(24MP 约 70ms/张,千张级项目扫一遍就是分钟级);AI 上传同源(F-02)。

方案: `core/decode` 增加 `decode_jpeg_file_bounded(path, max_px)`(`CGImageSourceCreateThumbnailAtIndex` + `kCGImageSourceThumbnailMaxPixelSize` + `kCGImageSourceCreateThumbnailFromImageAlways`,JPEG 解码器原生支持按 1/2/4/8 降尺度,耗时随目标像素走)。预取注入带上限的解码函数(上限取面板像素两倍或固定 4K 级,兼顾放大余量);dedup 的 decode_fn 换成 max_px≈64 的版本;AI 路径换 1568 版。数据先行: 先在真实素材上量当前 RSS 与 60MP JPEG 的实际 key-to-render,把收益写成数字再定上限。

**已确定(2026-07-11 拍板)**: 这轮不处理,维持"未实现"。报告自己的优先级就是 P2、且已明确"当前 24MP 内体验尚达标,不算急修",需要先在真实 24MP+/60MP 素材上量出数据才能定上限,不阻塞其它项目的排期。

难度 M | 复杂度 中 | 优先级 P2(内存与 60MP 数据已能立项,当前 24MP 内体验尚达标,不算急修)

---

**F-14 PrefetchCache::get 持锁整块拷贝像素**
类别: 性能/内存 | 来源视角: 工程

背景: prefetch.cpp:150 按值返回 `DecodedImage`(24MP = 96MB 拷贝),且发生在持有 mu_ 期间,拷贝时长内 worker 全阻塞。

方案: `Entry` 存 `std::shared_ptr<const decode::DecodedImage>`,get 返回 shared_ptr(拷贝指针不拷贝像素);调用方(browse.cpp)只读使用,改动极小。F-13 落地后此项收益缩水,可与之合并实施。

难度 S | 复杂度 低 | 优先级 P2

---

**F-15 set_current 每次按键为全项目构建路径表**
类别: 性能/内存 + 纯清理 | 来源视角: 工程

背景: prefetch.cpp:98-105 对 images 全量构建 `resolved_path_by_id`(O(n) 字符串分配),每个按键(含不导航的)都跑,且在延迟计时窗口之外(量不到)。

方案: 只为 `wanted`(窗口内 2w+1 个)解析路径;顺带把 key-to-render 计时起点挪到 read() 返回之后(与 F-36 的计时项合并处理)。

难度 S | 复杂度 低 | 优先级 P2

---

**F-16 抽 core/media: 消除 core/ai 对门面的反向依赖与三份 raw 扩展名判断**
类别: 纯重构 | 来源视角: 架构

背景: 见 3.2-A。行为完全不变的结构调整,消除"加新 RAW 格式时三处漂移"的真实陷阱。

方案: 新建 `core/media/`(或 core/decode 下平级文件): `is_raw_path()`、`decode_preview_file()`、`resolve_preview_path(root, file_path, kind, cache)`;api.cpp 改转调,evaluation_worker.cpp 去掉 `#include "core/api.h"`,dedup.cpp 删除本地复制品。单测原样通过即验收。

难度 S-M | 复杂度 低 | 优先级 P2

---

**F-17 写路径 sqlite3_step 返回值不检查(recipe.cpp 四处、evaluation_worker、project 两处)**
类别: 工程规范 + Bug | 来源视角: 工程

背景: 见 4.4。最实的一处: `create_version` 失败时返回上一条 insert 的 rowid 当新 id。project/tagging 已有"查了就 throw"的既有标准,同仓库两套做法。

方案: 统一补 `!= SQLITE_DONE` 检查并 throw(配合 F-05 的边界后,throw 是安全的);evaluation_worker 落库失败改走 F-03 的失败提示链路。

难度 S | 复杂度 低 | 优先级 P2

---

**F-18 dedup 打标签忽略 add_tag 返回值,汇总计数可能虚高**
类别: Bug(edge) + 工程规范 | 来源视角: 工程

背景: 见 4.1。触发条件是用户手建过带 cap 的同名"重复"标签,概率低但语义确实错。

方案: `add_tag(...).ok()` 才 `++tagged_count`;失败可累计进一个 `skipped_by_cap` 或至少 stderr 一行。

难度 S | 复杂度 低 | 优先级 P2

---

**F-19 Result<T,E> 强化: [[nodiscard]] 与 NDEBUG 下的取值防线**
类别: 工程规范 | 来源视角: 工程

背景: 见 4.4。`Result<void,E>::error()` 在 NDEBUG 下解引用空 optional 是 UB;F-18 证明"返回值被丢弃"已实际发生。

方案: 类模板与 Ok/Err 工厂、value()/error() 加 `[[nodiscard]]`;取值函数在断言编译掉时也 fail-fast(`std::abort` 或统一 throw)。全仓库编译一遍,把新暴露的告警逐个处理(预计就是 dedup 那两处)。

难度 S | 复杂度 低 | 优先级 P2

---

**F-20 终端 resize 无响应: SIGWINCH 未处理且无整屏清除,残影错位**
类别: Bug + 交互打磨 | 来源视角: 用户 + 工程

背景: 见 2.1 场景 E。布局每帧按最新尺寸计算,但重绘只由按键触发,且从不 `\x1b[2J`,旧边框永远没人擦。

方案: poll 循环里每轮比较 `get_terminal_size()` 与上一帧(比装信号 handler 简单且够用,poll 已因 debug/AI 存在);尺寸变化时全屏清除 + 强制重画图片(navigated 置真)。注意不开 debug 且无 AI 任务时当前是纯阻塞 read,需要把"有没有必要 poll"的条件加上"上次已知尺寸"这一档,或接受"resize 后按任意键恢复"并至少做好那一刻的整屏清除。两个分支都写在这,实施时择一。

难度 S-M | 复杂度 低 | 优先级 P2

---

**F-21 curl 加固: CURLOPT_NOSIGNAL 与连接超时**
类别: 工程规范 + 健壮性 | 来源视角: 工程

背景: 见 4.2。多线程进程里不设 NOSIGNAL 是经典雷区(DNS 超时走 SIGALRM);只有 60s 总超时,无 `CURLOPT_CONNECTTIMEOUT`,断网时 worker 会吊满 60 秒。

方案: `NOSIGNAL=1L`、`CONNECTTIMEOUT=10L`。两行。

难度 S | 复杂度 低 | 优先级 P2

---

**F-22 控制台/文本纯函数从 browse.cpp 匿名空间抽成可测模块**
类别: 纯重构 + 工程规范 | 来源视角: 架构 + 工程

背景: 见 3.2-F。`take_scope_token`(引号解析,本轮 E2E 刚修过的高危区)/`split_console_command`/`wrap_tokens` 零测试,原因纯粹是位置(编进可执行文件的 TU)。控制台命令还会继续增多。

方案: 抽 `cli/console`(依赖 cli/text/i18n)或先并入 cli/text;补 doctest 用例(引号闭合/未闭合、多空格、`#` 边界、CJK 宽度换行)。顺带记录: browse.cpp 已 1138 行,下一个大特性进来前先抽帧渲染函数,本条不含该项。

难度 S | 复杂度 低 | 优先级 P2

---

**F-23 AGENTS.md 状态段过期("M3 尚未开始")**
类别: 文档修正 | 来源视角: 架构

背景: 根 AGENTS.md"当前状态"仍写"Milestone 3(AI 辅助功能)尚未开始",而 M3 增量一(评估)与近似重复检测均已实现验收;"权威文档"清单也未列 M3 的四份文档。按项目"agent 必读文档"的定位,这份漂移会误导后续会话。

方案: 更新状态段(M3 增量一 + dedup 已完成,后续增量待启动),权威文档清单补 M3_PRD/M3_Eng_Design/M3_Dedup_PRD/M3_Dedup_Eng_Design。

难度 S | 复杂度 低 | 优先级 P2

---

**F-24 会话续点: 重开项目回到上次浏览位置**
类别: 需求proposal | 来源视角: 产品 + 用户

背景: 见 1.2。多次分批选片是真实使用模式(用户实际就这么用),每次重开都从最新一张开始。

方案: `projects` 加可空 `last_image_id` 列(ensure_column 迁移),`q` 退出时写入,open 时若该 id 仍在列表则作为起点。约 30 行 + 一条迁移。

难度 S | 复杂度 低 | 优先级 P2

---

**F-25 阻塞操作(/dedup、批量导出)结束后清空 tty 输入缓冲**
类别: Bug + 交互打磨 | 来源视角: 用户 + 工程

背景: 见 4.2。冻结期间的按键解冻后连发回放,可能误标/误退。Dedup PRD 风险清单遗留的"实现阶段需真机验证并记录"从未收口。

方案: 长阻塞调用返回后 `tcflush(STDIN_FILENO, TCIFLUSH)`(dedup、g e 大批量导出、e 单张 RAW 导出三处);顺手在 PRD 风险条目里记上结论。

难度 S | 复杂度 低 | 优先级 P2

---

**F-26 批量操作默认排除废片/重复(/ai_eval、/dedup、export)**
类别: 需求proposal + 交互打磨 | 来源视角: 产品 + 用户

背景: 见 2.1 场景 D。已废弃的照片被评估是白花钱,被 dedup 拉进组里甚至可能当 keep_id。讨论中范围从最初只提的"dedup 排除废片"扩大到 eval/dedup/export 三处统一处理,并且加入了"范围本身就是废片/重复标签时不排除"这个对称例外。

**已确定方案**(2026-07-11 拍板,范围明显扩大):

* `/ai_eval`、`/dedup` 的批量范围(`*` 和 `#标签`)默认排除带"废片"标签的图,除非范围标签本身就是 `#废片`(这种情况下用户显式要求处理废片,不再排除)——两条命令各自受一个独立开关控制(`Settings.eval_reject`/`dedup_reject`),不是共享同一个开关。
* `pzt export`/`g e` 默认排除"废片"和"重复"标签的图,除非要导出的目标标签本身就是废片或重复(`Settings.export_reject`/`export_dup`)。
* 单张 `e` 导出(浏览时导出当前这张)**不过滤**——用户当下正在看的就是这张图,按 e 是明确的单张意图,不应该因为这张图恰好打了废片/重复就拒绝导出。
* 这几个开关目前只能通过 F-12 的 `Settings`(静态配置文件)调,没有运行时切换入口——运行时 `/setting eval_reject true` 这类开关是明确的未来任务,见 F-12"未来任务"一节。

技术方案:

* 新增 `core::tagging::images_with_tag(db, image_ids, tag_id) -> unordered_set<ImageId>`: 一条 `WHERE image_id IN (...) AND tag_id = ?` 查询,不是逐张查(避免 N+1)。这是本条和 F-09 共用的底层原语。
* **ai_eval / dedup 批量范围**(`resolve_console_scope` 所在的 `cli/commands/browse.cpp`): `ScopeResolution` 结构体新增 `std::optional<TagId> scope_tag_id`(`*` 时为空,`#tag` 时是解析出的标签 id)。`handle_ai_eval_command`/`handle_dedup_command` 里,`resolve_console_scope` 返回后,若 `!settings.eval_reject`(或 `dedup_reject`)且 `scope_tag_id != reject_tag_id`,调用 `images_with_tag` 排除废片。
* **export**(`core::exporting::export_tag`,`core/export/export.h`/`.cpp`): 签名新增 `bool include_reject = false, bool include_dup = false`。内部解析完 `browse::filter_by_tag(tag_id)` 之后,若 `tag_id != reject_tag_id && !include_reject`,用 `images_with_tag` 排除废片;`tag_id != duplicate_tag_id && !include_dup` 同理排除重复。`core/api.h` facade 和 `cmd_export`/`handle_g_export_flow` 两个调用点透传 `Settings` 里的 `export_reject`/`export_dup`。

依赖 F-12(需要 `Settings` 结构体里的四个开关字段)。

难度 M | 复杂度 中(比原方案范围更大,涉及三个消费点 + 一个新 core 原语) | 优先级 P2

### P3

---

**F-27 信息栏显示"未标记剩余 N 张"**
类别: 交互打磨 | 来源视角: 用户
j/k 工作流的进度感。一条 COUNT 查询 + 一行展示;注意别把它做成每帧全表扫描(与 F-38 的重测一起看)。
难度 S | 复杂度 低 | 优先级 P3

**F-28 有序标签没有调序 UI(九宫格故事只兑现一半)**
类别: 需求proposal + 观察记录 | 来源视角: 产品
position 语义存在(replace 继位、导出按位编号)但顺序只能等于打标签顺序。分支: (a) 筛选视图内加"上移/下移"操作(M);(b) 明确文档化"顺序 = 打标签顺序",降级为已知边界。先拍板,不默认实现。
难度 M | 复杂度 中 | 优先级 P3

**F-29 补 unarchive**
类别: 需求proposal | 来源视角: 用户
M0 起挂账的对称缺口。core 一个 UPDATE + cli 一个子命令。
难度 S | 复杂度 低 | 优先级 P3

**F-30 version 改名只有 CLI 寻址语法,无应用内入口**
类别: 交互打磨 + 观察记录 | 来源视角: 用户
低频;等真实使用出现再决定加 r 菜单 rename 还是维持现状。
难度 S | 复杂度 低 | 优先级 P3

**F-31 删除门面死代码 core::find_image_by_path**
类别: 纯清理 | 来源视角: 工程
服务对象(调试标签命令)在 6.4.7 已退休,cli 无调用方(已 grep 确认);内层 project:: 版本若仅测试使用一并评估。
难度 S | 复杂度 低 | 优先级 P3

**F-32 ensure_default_presets 每次门面调用重算 Warm LUT**
类别: 纯清理 + 性能/内存 | 来源视角: 工程
见 3.3。播种挪到 Database 初始化后执行一次,或 seed 前 SELECT 检查。
难度 S | 复杂度 低 | 优先级 P3

**F-33 ensure_column_dropped 四条永久迁移残留**
类别: 纯清理 | 来源视角: 工程
只服务 M3 开发期中间态,真实库已迁移完;每次开库白跑 4 次 PRAGMA。个人工具可直接删;保守起见留注释注明"某日期后可删"。
难度 S | 复杂度 低 | 优先级 P3

**F-34 raw 解码 RGBA 第 4 字节填 0 改 255**
类别: 工程规范 | 来源视角: 工程
见 4.4。防御 f=32(alpha 有效)路径未来直连时的全透明陷阱。
难度 S | 复杂度 低 | 优先级 P3

**F-35 is_wide_codepoint 补 emoji 区间**
类别: Bug(edge) | 来源视角: 工程
0x1F300-0x1FAFF 视作宽字符;文件名带 emoji 时边框不再错位。
难度 S | 复杂度 低 | 优先级 P3

**F-36 工程规范杂项打包**
类别: 工程规范 | 来源视角: 工程
一次顺手修完: write_stdout 短写循环;usleep 换 sleep_for;key-to-render 计时起点挪到 read() 后(现在漏掉按键处理与 set_current);p95 索引小样本偏差;compute_dhash 失败返回 0 改 optional(现路径不可达,纯防御)。
难度 S | 复杂度 低 | 优先级 P3

**F-37 api.h 头部重量: nlohmann/thread 透传给全部 cli TU**
类别: 纯重构 + 观察记录 | 来源视角: 架构
api.h -> evaluation_worker.h -> ai.h -> nlohmann/json.hpp,i18n.h 又 include api.h,全 cli 编译单元都在解析 json 头。工程规模尚小,等编译时间成为体感问题再动(方案: worker pimpl 或 api.h 拆分)。
难度 M | 复杂度 低 | 优先级 P3

**F-38 门面每帧多次开连接: 重测数字,维持观察**
类别: 观察记录 + 性能/内存 | 来源视角: 架构
Optimization_Backlog 既有条目。schema init 变厚后建议重测一次每帧真实开销(RelWithDebInfo 下 key-to-render 是否仍在 60ms 档);热点已由 F-07 单独消灭。若 M4 headless 层立项,顺势引入持连接的 core::Session,现在不预做。
难度 -(测量) | 复杂度 低 | 优先级 P3

**F-39 dedup 分组输出顺序确定化**
类别: 纯清理 | 来源视角: 工程
unordered_map 遍历序不稳定,违反 Dedup PRD"确定性"NFR 字面(打标签集合本身一致)。输出前按组内最小 id 排序。
难度 S | 复杂度 低 | 优先级 P3

**F-40 IN 子句按 500 分块(SQLITE_MAX_VARIABLE_NUMBER 防线)**
类别: 健壮性(edge) | 来源视角: 工程
3 万张以上项目才会触发;与 F-07 的批量查询一起实现即可,不单独排期。
难度 S | 复杂度 低 | 优先级 P3

**F-41 pzt list 缺各标签计数(M0 PRD 字面承诺)**
类别: 文档修正 或 Bug | 来源视角: 产品
二选一: 实现(list_projects 加聚合,S)或修订 PRD 措辞。倾向后者(应用内 space 菜单已带计数,list 保持轻)。
难度 S | 复杂度 低 | 优先级 P3

**F-42 read_text_line(无 placeholder 版)超宽不换行,光标越出边框**
类别: Bug(edge) + 交互打磨 | 来源视角: 工程
长导出路径输入时体验不佳;对齐 placeholder 版已有的两行换行逻辑(两个函数本就共享编辑状态机,redraw 统一即可)。
难度 S | 复杂度 低 | 优先级 P3

**F-43 导出路径记忆**
类别: 交互打磨 | 来源视角: 用户
`e`/`g e` 的路径输入预填上一次用过的目录(会话内存级即可,持久化并入 F-12 配置)。
难度 S | 复杂度 低 | 优先级 P3

**F-44 对当前筛选视图批量打标签**
类别: 需求proposal | 来源视角: 用户
"筛出不达标 -> 全部打废片"这类批量动作的通用形态;与 F-09(c) 有重叠,等 F-09 用出手感后一并设计,避免两套批量语义。
难度 M | 复杂度 中 | 优先级 P3

**F-45 Esc 消歧 20ms 阈值在高延迟终端(ssh)下会把方向键误判成 Esc**
类别: 观察记录 | 来源视角: 工程
项目环境锁定本地 Ghostty,记录即可;若未来支持远程,改读 ESC 后跟随字节的状态机(不依赖时延)。
难度 - | 复杂度 - | 优先级 P3

**F-46 渲染失败时 tmpfile 残留 / t=s 共享内存介质验证**
类别: 观察记录 | 来源视角: 工程
WriteFailed 分支会留下临时文件(正常路径由终端读后删除);t=s 是 M0 起挂账的可选优化。均维持既有 backlog 状态。
难度 - | 复杂度 - | 优先级 P3

**F-47 纯 JPEG 大项目导入无进度输出**
类别: 交互打磨 | 来源视角: 用户
逐张读 EXIF(约 2ms/张)在千张级要数秒,期间无输出。复用 ScanProgressFn 语义加一档"扫描中 X/N"(注意与 RAW 缓存进度的文案区分)。
难度 S | 复杂度 低 | 优先级 P3

**F-48 低频功能盘点结论(不动)**
类别: 观察记录 | 来源视角: 用户
`pzt tag list`、`r v`、`--no-prune`、cap 替换流维持现状;见 2.3 表格。此条仅留档,无行动。
难度 - | 复杂度 - | 优先级 P3

---

## 六、汇总矩阵

| 编号 | 一句话 | 类别 | 来源视角 | 难度 | 复杂度 | 优先级 | 当前状态 |
|---|---|---|---|---|---|---|---|
| F-01 | "重复"标签菜单不可达,dedup 复核链断裂 | Bug | 产品+用户+工程 | S-M | 低 | **P0** | 已完成 |
| F-02 | AI 评估上传全分辨率原图 | Bug+性能 | 工程+用户 | S | 低 | P1 | 已完成 |
| F-03 | AI 失败静默,用户无从得知 | Bug+交互 | 用户+工程 | S | 低 | P1 | 已完成 |
| F-04 | 无 busy_timeout,并发写可 terminate | 健壮性 | 工程+架构 | S | 低 | P1 | 已完成 |
| F-05 | 无异常边界,崩溃留坏终端 | 健壮性 | 工程+架构 | S | 低 | P1 | 已完成 |
| F-06 | 扫描目录异常直接崩溃 | 健壮性+Bug | 工程+用户 | S | 低 | P1 | 已完成 |
| F-07 | 批量命令 N+1 连接,含 dedup 冗余统计 | 性能+纯清理 | 工程+架构 | S-M | 低 | P1 | 已完成 |
| F-08 | dedup 参数写死/无时间图静默跳过/无观测 | Bug+需求 | 用户+产品+工程 | M | 中 | P1 | 设计已定稿,未实现 |
| F-09 | 评估结果不可行动(控制台二级筛选) | 需求proposal | 产品+用户 | M | 中 | P1 | 设计已定稿,未实现 |
| F-10 | 供应商硬编码,Claude 不可达 | 需求+纯清理 | 产品+工程 | S | 低 | P1 | 已完成 |
| F-11 | dedup 后无复核入口动线 | 交互打磨 | 产品+用户 | S | 低 | P1 | 未实现 |
| F-12 | 统一全局设置系统 | 需求proposal | 产品+架构 | M | 中 | P1 | 设计已定稿,未实现 |
| F-13 | 有界解码(内存/60MP/dedup 提速) | 性能/内存 | 工程+架构+产品 | M | 中 | P2 | 已拍板推迟 |
| F-14 | prefetch get 持锁拷贝整图 | 性能/内存 | 工程 | S | 低 | P2 | 未实现 |
| F-15 | set_current 全项目路径表 O(n)/键 | 性能+纯清理 | 工程 | S | 低 | P2 | 未实现 |
| F-16 | 抽 core/media,消依赖倒挂与三份拷贝 | 纯重构 | 架构 | S-M | 低 | P2 | 未实现 |
| F-17 | 写路径 sqlite3_step 不检查 | 工程规范+Bug | 工程 | S | 低 | P2 | 已完成 |
| F-18 | dedup 忽略 add_tag 结果 | Bug(edge) | 工程 | S | 低 | P2 | 已完成 |
| F-19 | Result 加 [[nodiscard]]/NDEBUG 防线 | 工程规范 | 工程 | S | 低 | P2 | 已完成 |
| F-20 | resize 残影,无 SIGWINCH 处理 | Bug+交互 | 用户+工程 | S-M | 低 | P2 | 未实现 |
| F-21 | curl NOSIGNAL/连接超时 | 工程规范 | 工程 | S | 低 | P2 | 已完成 |
| F-22 | 控制台纯函数抽库补测 | 纯重构+规范 | 架构+工程 | S | 低 | P2 | 未实现 |
| F-23 | AGENTS.md 状态过期 | 文档修正 | 架构 | S | 低 | P2 | 已完成 |
| F-24 | 会话续点 | 需求proposal | 产品+用户 | S | 低 | P2 | 未实现 |
| F-25 | 阻塞后清 tty 输入缓冲 | Bug+交互 | 用户+工程 | S | 低 | P2 | 已完成 |
| F-26 | 批量操作默认排除废片/重复(eval+dedup+export) | 需求+交互 | 产品+用户 | M | 中 | P2 | 设计已定稿,未实现 |
| F-27 | 未标记剩余计数 | 交互打磨 | 用户 | S | 低 | P3 | 未实现 |
| F-28 | 有序标签调序缺口(先拍板) | 需求+观察 | 产品 | M | 中 | P3 | 未实现 |
| F-29 | unarchive | 需求proposal | 用户 | S | 低 | P3 | 未实现 |
| F-30 | version 应用内改名缺口 | 交互+观察 | 用户 | S | 低 | P3 | 未实现 |
| F-31 | 删门面 find_image_by_path 死代码 | 纯清理 | 工程 | S | 低 | P3 | 已完成 |
| F-32 | 预设播种每调用重算 LUT | 纯清理+性能 | 工程 | S | 低 | P3 | 已完成 |
| F-33 | 迁移残留 ensure_column_dropped x4 | 纯清理 | 工程 | S | 低 | P3 | 已完成 |
| F-34 | raw alpha 字节 0 改 255 | 工程规范 | 工程 | S | 低 | P3 | 未实现 |
| F-35 | 宽度表补 emoji | Bug(edge) | 工程 | S | 低 | P3 | 未实现 |
| F-36 | 规范杂项包(短写/usleep/计时/p95等) | 工程规范 | 工程 | S | 低 | P3 | 未实现 |
| F-37 | api.h 头部重量 | 纯重构+观察 | 架构 | M | 低 | P3 | 未实现 |
| F-38 | 门面连接开销重测(维持观察) | 观察+性能 | 架构 | - | 低 | P3 | 未实现 |
| F-39 | dedup 分组顺序确定化 | 纯清理 | 工程 | S | 低 | P3 | 未实现 |
| F-40 | IN 子句分块防变量上限 | 健壮性(edge) | 工程 | S | 低 | P3 | 未实现 |
| F-41 | pzt list 标签计数 drift 收口 | 文档修正 | 产品 | S | 低 | P3 | 未实现 |
| F-42 | read_text_line 超宽换行 | Bug(edge) | 工程 | S | 低 | P3 | 未实现 |
| F-43 | 导出路径记忆 | 交互打磨 | 用户 | S | 低 | P3 | 未实现 |
| F-44 | 筛选视图批量打标签 | 需求proposal | 用户 | M | 中 | P3 | 未实现 |
| F-45 | Esc 20ms 阈值远程场景 | 观察记录 | 工程 | - | - | P3 | 未实现 |
| F-46 | tmpfile 残留 / t=s 验证 | 观察记录 | 工程 | - | - | P3 | 未实现 |
| F-47 | 纯 JPEG 导入进度 | 交互打磨 | 用户 | S | 低 | P3 | 未实现 |
| F-48 | 低频功能盘点(维持现状) | 观察记录 | 用户 | - | - | P3 | 未实现 |

## 七、建议执行顺序

**第一晚快修包**(全部 S 难度、低风险,一晚可清完;顺序即依赖序):
F-05(异常边界,先立保险) -> F-04(busy_timeout) -> F-06(扫描健壮性) -> F-02(AI 上传缩放) -> F-03(失败提示) -> F-10(供应商环境变量最小版) -> F-17 + F-18 + F-19(写路径与 Result 一起收口) -> F-25(tcflush) -> F-21(curl 两行) -> 清理三连 F-31/F-32/F-33 -> F-23(文档)。

**需要先拍板再动手**(每条给出了分支,拍板后多为 S-M):
* F-01: 选 9 号位方案(推荐 a)
* F-26: 批量范围是否默认排除废片(推荐 a)
* F-08: dedup 参数暴露形态(命令参数 vs 等 F-12 配置)
* F-12: 配置文件格式与第一批键
* F-09: 评估结果筛选的形态(g 菜单虚拟项 vs 控制台命令)
* F-13: 有界解码的上限策略(先测数字)

**正常排期**(P2 里非快修的): F-07 -> F-16 -> F-22 -> F-20 -> F-24 -> F-14/F-15 -> F-13。

**留档观察**: P3 其余条目,按"真实需求/数据触发"原则处理。

## 八、执行日志(第一晚快修包)

本节记录 2026-07-11 执行第一晚快修包(第七节列出的 11 组、共 15 个 F 编号)的过程。执行前先扫过一遍这批任务是否有需要拍板的分支决策——第七节"需要先拍板"那一段列的 6 条(F-01/08/09/12/13/26)本来就不在快修包范围内,快修包里的 15 项在报告写作时已经把方案定死(全部 S 难度、方案单一),不需要用户拍板,直接按顺序执行。

### 执行方式

每个任务走同一个四步循环:(1) 确认工作树干净、把上一项的改动先提交;(2) 写代码 + 配套单元测试;(3) `cmake --build` + `ctest` 跑 `build/`(Debug)和 `build_release/`(RelWithDebInfo)两个配置,涉及真实交互路径(cli 按键循环、AI 网络请求)的另外用 `expect` 驱动一个真实 pty 会话做端到端验证;(4) 在第六节矩阵里把对应行的"当前状态"改成"已完成",单独提交。全程 15 项按报告"建议执行顺序"给出的依赖序完成,没有跳过或搁置任何一项,也没有发现需要中途改变方案的情况。

### 逐项结果

| 编号 | 结果 | 关键验证方式 |
|---|---|---|
| F-05 | 已完成 | 新增 `err_internal_error` i18n 用例;`main()` 正常路径(未知子命令)手动回归 |
| F-04 | 已完成 | 新增 `PRAGMA busy_timeout;` 断言用例,确认每条新连接都带非零超时 |
| F-06 | 已完成 | 新增"目录不存在"单元测试;pty/CLI 手动跑三种场景(拼错 flag、目录不存在、合法 `--support-raw`)均不崩溃 |
| F-02 | 已完成 | 新增 `detail::downscale_for_upload` 单元测试(验证等比缩放与不缩小两种边界) |
| F-03 | 已完成 | 新增 `take_last_failure` 系列单元测试(网络失败、成功路径、图片不存在三种场景);i18n 文案随语言切换的用例 |
| F-10 | 已完成 | pty 真实网络请求验证:`PZT_AI_PROVIDER=claude` 命中真实 Claude API(带假 key 拿到 401,证明确实发到了 Claude 而不是 Gemini);不设该变量时回退 Gemini 的行为保持不变 |
| F-17+F-18+F-19 | 已完成 | `recipe.cpp` 四个写函数补齐失败检查;`evaluation_worker.cpp` 落库失败改走 F-03 的失败通道(新增 `EvaluationError::StorageFailed`,新增用只读文件权限强制触发真实写失败的单元测试);`project.cpp` 两处 best-effort 回写改为失败时打日志、不中断批处理;`dedup.cpp` 的 `add_tag` 忽略返回值问题修复(新增 cap 冲突场景单测);`Result<T,E>` 整个类标 `[[nodiscard]]`,`value()`/`error()` 改为不依赖 `NDEBUG` 的 fail-fast;全仓库干净重编两个配置,逐一处理新出现的 3 处丢弃警告(均为显式 `(void)` 丢弃 + 注释,不是新增复杂逻辑) |
| F-25 | 已完成 | 新增 `cli::ui::flush_pending_input()`,接入三处长阻塞调用;pty 验证 `/dedup *` 全流程正常完成并恢复到空闲状态 |
| F-21 | 已完成 | pty 真实网络请求验证:设置 `CURLOPT_NOSIGNAL`/`CURLOPT_CONNECTTIMEOUT` 后,真实请求（带假 key）仍然正常发出并拿到服务端响应 |
| F-31+F-32+F-33 | 已完成 | 删除死代码前先用 `grep` 确认零调用方；F-33 额外查询了真实的 `~/.config/pzt/pzt.db`,确认迁移早已生效,再动手删 |
| F-23 | 已完成 | 纯文档改动,更新权威文档清单与状态段,人工核对措辞准确性 |

### 构建与测试结果

15 项全部完成后,`build/`(Debug + ASan/UBSan)与 `build_release/`(RelWithDebInfo)两个配置从零 `cmake --build` 均为**零警告**（`-Wall -Wextra` 下,排除跟本次改动无关的链接器"重复库"提示）,`ctest` 在两个配置下均为 **100% 通过**(`core_tests`/`cli_tests` 两个可执行文件)。过程中因为 `[[nodiscard]]` 标注在全仓库重编一次时新发现 3 处此前从未被注意到的返回值丢弃(`dedup.cpp` 一处、`browse.cpp` 两处),均已定位、评估、显式处理并验证,不是遗留问题。

### 与计划的偏差

* **F-17 的执行范围略微超出报告原文字面**:报告原文只点名了 `recipe.cpp` 四处、`evaluation_worker.cpp` 一处、`project.cpp` 的"backfill/缓存路径回写"若干处；实际执行时,`evaluation_worker.cpp` 那一处没有照搬 `recipe.cpp` 的"throw"模式——落库失败发生在后台 `jthread` 上,直接 throw 会在没有异常边界保护的后台线程触发 `std::terminate`,风险比"结果暂时没存上"本身更大。改为复用同一晚 F-03 刚建好的 `take_last_failure()` 失败通道,新增了一个 `EvaluationError::StorageFailed` 枚举值——这比报告原文设想的"统一 throw"更贴合实际约束,是执行阶段发现问题后现场调整的方案,不是偏离任务本身。
* **F-33 从"两个分支二选一"收敛成了单一决定**:报告给了"直接删"和"保留但加注释"两个分支。执行时直接查询了真实数据库确认迁移早已完成,证据确凿,选择了"直接删"这个更彻底的分支,不是走了折中方案。
* 其余 13 项均按报告原文的单一方案原样落地,没有需要临时调整的地方。

### 未变化的部分

第六节矩阵里其余 33 项(F-01、F-07、F-08、F-09、F-11~F-16、F-20、F-22、F-24、F-26~F-30、F-34~F-48)均维持"未实现"，本轮未触碰。其中 F-01 是报告标记的唯一 P0,需要在下一轮明确拍板"9 号固定给重复标签"这类交互设计决策后才能动手,不在"直接执行"的范围内。

## 九、后续排期

2026-07-11 晚上继续过了一轮"需要先拍板"的六项(F-01/F-08/F-09/F-12/F-13/F-26),逐条决策已经写回上面对应的"逐项清单"条目(标"已确定方案"或"已拍板推迟"),这次**只记录方案,不写代码**——第六节矩阵这六项状态相应改成"设计已定稿,未实现"(F-13 是"已拍板推迟")。

用户随后指出一个关键点:开始按"建议执行顺序"(第七节)里的"正常排期(P2 里非快修的)"往下做之前,应该先确认所有 P0/P1 是不是已经收尾——检查发现并没有:第一晚快修包只覆盖了 P0/P1 里难度 S、方案单一的部分,F-01(唯一 P0)和 F-07/F-08/F-09/F-11/F-12(P1)都还没实现。这些必须先排进去,不能跳过直接进第七节的 P2 列表。

### 依赖关系

* F-01、F-07、F-12 三项互相独立,谁先谁后都可以,且都已经有明确到函数签名级别的方案(F-07 是原报告就写好的,不需要再拍板;F-01/F-12 是这次刚拍板的)。
* F-11 依赖 F-01(要先有能看到"重复"标签的入口,"跳去复核"这个提示才有意义)。
* F-26(P2,但技术上是 F-09 的前置)、F-08 都依赖 F-12(要先有 `Settings` 结构体)。
* F-09 依赖 F-26 新增的 `core::tagging::images_with_tag`——虽然 F-26 本身标 P2,但 F-09(P1)用得上它,技术依赖不跟着优先级走,这里按依赖排,不按 P0/P1/P2 硬排。

### 建议顺序(仍然遵循"建代码 -> 配测试 -> build 两个配置 -> 提交"的节奏,一项一提交)

**第一批(地基,互不依赖,可任意顺序)**:
1. **F-01**(唯一 P0,优先做完)——`tags_for_menu` 截断改 8、三处菜单加 `duplicate_tag_id` 参数和 `9:重复` 分支。
2. **F-07**——门面加批量查询 `evaluated_subset`,替换 `handle_ai_eval_command`/`handle_dedup_command` 里的 N+1 循环,删除 `DedupSummary::unevaluated_image_count` 死字段。
3. **F-12**——`core/settings` 模块 + `core/api.h` facade,这轮不含 `/setting` 运行时开关。

**第二批(依赖第一批)**:
4. **F-11**(依赖 F-01)——dedup 结果文案加入口提示。
5. **F-26**(依赖 F-12)——新增 `images_with_tag`,eval/dedup/export 三处接入默认排除逻辑。
6. **F-08**(依赖 F-12)——dedup 阈值改读 `Settings`,加 `skipped_no_capture_time` 计数和 `--debug` 汉明距离日志。

**第三批(依赖第二批,改动面最广,建议单独一次会话)**:
7. **F-09**(依赖 F-26 的 `images_with_tag`)——控制台 `/filter` 二级筛选,控制台分发返回类型改造 + `cmd_open` 状态机扩展 + 信息栏展示。

**这批全部完成之后**,P0/P1 才算清空,再按第七节"正常排期"继续 F-16 -> F-22 -> F-20 -> F-24 -> F-14/F-15(F-07 已经在上面第一批处理过,第七节原文那条不用重复做)。F-13 已拍板推迟,继续留在矩阵里等真实素材数据。
