# PZT Fix-it Night 剩余项(2026-07 评审的未完成 backlog)

> 本文是 2026-07 Fix-it Night 全面评审(基于 commit `8f5af14`,编号 F-01 到 F-48)中**尚未完成**的条目。已完成的 22 条、完整执行日志、以及原始四视角评审分析已抽离归档到 `docs/history/Fix_It_Night_2026-07_Completion_Report.md`,需要追溯"某条 finding 当初怎么发现的"或"某条已完成项怎么落地的"时去那里查。
>
> **2026-07-16 复审**:结合本周(W2026-07-15:本地模型 Ollama / recipe 扩展 / agent Style)以来的代码改动重新过了一遍这些条目。相关文件 `cli/commands/browse.cpp`、`core/dedup/dedup.cpp`、`core/browse/prefetch.cpp`、`core/raw/raw.cpp`、`cli/text/*` 本周均未改动,绝大多数条目的证据(file:line)与结论保持有效;个别随代码漂移或被本周改动波及的引用已就地更新(F-22 行数、F-37 头文件、F-38 schema、F-40/F-44 依赖项)。本轮**只做 update 与清理,不新增问题**(新问题属于下一次 Fix-it Night 的范围)。
>
> 条目正文里的 "见 N.M"(如 "见 3.2-A"、"见 2.1 场景 E"、"见 4.3")指向已归档到完成报告第五节的原始四视角评审分析对应小节;"见 3.3"/"见 2.3 表格" 同理。

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

## 逐项清单(未完成)

### P2

---

**F-13 有界解码(subsampled decode): 内存、60MP 秒切、dedup 提速的统一杠杆**
类别: 性能/内存 | 来源视角: 工程 + 架构 + 产品

背景: 见 4.3 与 1.3-3。预取缓存存全分辨率 RGBA(24MP x 7 张约 0.7GB;60MP 单张解码约 180ms 直接超预算,M0 已知 gap);dedup 为了 9x8 哈希做全量解码(24MP 约 70ms/张,千张级项目扫一遍就是分钟级);AI 上传同源(F-02)。

方案: `core/decode` 增加 `decode_jpeg_file_bounded(path, max_px)`(`CGImageSourceCreateThumbnailAtIndex` + `kCGImageSourceThumbnailMaxPixelSize` + `kCGImageSourceCreateThumbnailFromImageAlways`,JPEG 解码器原生支持按 1/2/4/8 降尺度,耗时随目标像素走)。预取注入带上限的解码函数(上限取面板像素两倍或固定 4K 级,兼顾放大余量);dedup 的 decode_fn 换成 max_px≈64 的版本;AI 路径换 1568 版。数据先行: 先在真实素材上量当前 RSS 与 60MP JPEG 的实际 key-to-render,把收益写成数字再定上限。

**已确定(2026-07-11 拍板)**: 这轮不处理,维持"未实现"。报告自己的优先级就是 P2、且已明确"当前 24MP 内体验尚达标,不算急修",需要先在真实 24MP+/60MP 素材上量出数据才能定上限,不阻塞其它项目的排期。

难度 M | 复杂度 中 | 优先级 P2(内存与 60MP 数据已能立项,当前 24MP 内体验尚达标,不算急修)

---

### P3

---

**F-27 信息栏显示"未标记剩余 N 张"**
类别: 交互打磨 | 来源视角: 用户
j/k 工作流的进度感。一条 COUNT 查询 + 一行展示;注意别把它做成每帧全表扫描(与 F-38 的重测一起看)。
难度 S | 复杂度 低 | 优先级 P3

---

**F-29 补 unarchive**
类别: 需求proposal | 来源视角: 用户
M0 起挂账的对称缺口。core 一个 UPDATE + cli 一个子命令。
难度 S | 复杂度 低 | 优先级 P3

---

**F-30 version 改名只有 CLI 寻址语法,无应用内入口**
类别: 交互打磨 + 观察记录 | 来源视角: 用户
低频;等真实使用出现再决定加 r 菜单 rename 还是维持现状。
难度 S | 复杂度 低 | 优先级 P3

---

**F-36 残留子项: key-to-render 计时起点挪到 read() 后**
类别: 观察记录(原工程规范) | 来源视角: 工程
F-36 打包的其余四项(write_stdout 短写循环、usleep→sleep_for、p95 最近秩、compute_dhash 返 optional)已完成(见"已完成"节)。**唯独计时起点这一项拍板不做**:分析发现 `prefetch.get()` 与 kitty 渲染本就在测量窗口内(帧顶 `navigated` 门控),导航键(h/l/j/k)从 read() 后计时只多算微秒级的 prev/next 扫描 + 非阻塞 set_current,收益可忽略;而 x 键有 150ms 故意闪烁、submit 有 800ms 提示这类**故意 UI 停顿**在按键处理里,把起点挪到 read() 前会把它们折进 key-to-render,x 又是选片最高频键,会让退出汇总的 avg/p95 失真、不再反映真实切图延迟。维持 key_time 在循环顶部。若未来要更严格测导航延迟,应按键分类单独计时,不是简单挪起点。
难度 - | 复杂度 - | 优先级 P3(留档,拍板不做)

---

**F-37 api.h 头部重量: nlohmann/thread 透传给全部 cli TU**
类别: 纯重构 + 观察记录 | 来源视角: 架构
api.h -> evaluation_worker.h -> ai.h -> nlohmann/json.hpp,i18n.h 又 include api.h,全 cli 编译单元都在解析 json 头。本周(W2026-07-15)agent Style/curate 落地后 api.h 又新增了 `core/curate/curate.h`、`core/settings/settings.h` 等重导出(见 core/api.h 顶部 include 列表),头只增不减、观察结论不变。工程规模尚小,等编译时间成为体感问题再动(方案: worker pimpl 或 api.h 拆分)。

**2026-07-19 Wave 2 实测(维持观察,不动)**: 用 release 编译标志(`-O2 -std=c++20`)量单 TU 编译:空 TU 0.038s,含 `core/api.h` 的 TU 0.694s → **api.h 头解析 ~0.66s/TU**(`-H` 头树确认拉入 nlohmann/json.hpp);含 `cli/i18n/i18n.h` 的 TU 0.701s(经 i18n 间接吃到同一份 json)。13 个 cli TU 里 **8 个透传 api.h**,合计约 `8×0.66≈5.3s` 的纯 json 头解析 CPU。一次 clean cli 全量重编(pzt + cli_tests,`-j` 并行)**8.6s wall**。结论:头开销真实可测,但绝对量仍小——clean 全量 <10s,日常增量只重编改动的那 1-2 个 TU(各 ~0.7s),**未越"体感问题"阈值**。触发条件:cli TU 数或 header-cascade(改一个 api.h 依赖的头就波及 8 个 TU ≈5s)增长到影响 edit-build 循环时再动;届时 worker pimpl 让 ai.h/json.hpp 停止透传,可从 8 个 TU 各砍 ~0.66s。
难度 M | 复杂度 低 | 优先级 P3

---

**F-38 门面每帧多次开连接: 重测数字,维持观察**
类别: 观察记录 + 性能/内存 | 来源视角: 架构
Optimization_Backlog 既有条目。schema init 变厚后建议重测一次每帧真实开销(RelWithDebInfo 下 key-to-render 是否仍在 60ms 档);热点已由 F-07 单独消灭。注:开库语句数本周又有变动——recipe 扩展新增 5 条 `ensure_column`(grain_amount/contrast/saturation/blacks/whites),F-33 删掉 4 条 `ensure_column_dropped`,重测时以当前 schema 为准。若 M4 headless 层立项,顺势引入持连接的 core::Session,现在不预做。

**2026-07-19 Wave 2 实测(维持观察,不动)**: 用 release 构建的 `libcore.a` 直接测 `Database::open_at`(= `open_default`,含 `sqlite3_open` + `initialize_schema` 全量迁移 + dtor close),对象是真实默认库的拷贝(648KB):warmup 后 2000 次 **avg 184µs / p50 179µs / p95 195µs**。当前 schema(7 CREATE TABLE + 13 `ensure_column` + 2 index)全走一遍就是这个数。渲染路径里门面开连接的调用**都是 current_ref 单张、O(1)**(非 O(images) 循环):一个导航帧约 5 次开连接(`tags_for_image`/`get_image`/`get_image_recipe`×2/`describe_recipe`),`passes_gate`/`overall_score`/`get_project_summary` 是纯计算不开连接。**每帧 DB 开销 ≈ 5×184µs ≈ 0.9ms**,占 60ms key-to-render 预算 ~1.5%,recipe +5 列没让指标移动。结论:仍可忽略,维持观察;真要消掉,等 M4 headless 立项时引 `core::Session` 持连接一次性解决,现在不预做。
难度 -(测量) | 复杂度 低 | 优先级 P3

---

**F-43 导出路径记忆**
类别: 交互打磨 | 来源视角: 用户
`e`/`g e` 的路径输入预填上一次用过的目录(会话内存级即可,持久化并入 F-12 配置)。
难度 S | 复杂度 低 | 优先级 P3

---

**F-44 对当前筛选视图批量打标签**
类别: 需求proposal | 来源视角: 用户
"筛出不达标 -> 全部打废片"这类批量动作的通用形态。F-09 的 `/filter` 二级筛选已落地(见完成报告),提供了"筛出"这一半;"对筛选结果批量打标签"这另一半仍未做,等 `/filter` 用出手感后一并设计,避免两套批量语义。

**2026-07-19 拍板:维持推迟,触发前提写清(下同)**。不现建(M 工量、暂无使用信号),但把"用出手感要回答的问题"固定下来,让推迟可操作——真机用 `/filter` 一段时间后能明确回答以下几点,就可立项(先出 F-44 PRD 再 Eng Design):
1. **入口与触发**:批量打标签是控制台命令(如 `/tag <name>` 呼应 `/filter`)还是一个新按键?跟现有 space 单张打标签、`x` 废片直达如何不冲突、不误触?
2. **作用域边界**:批量作用于"当前 `images` 浏览池"(= g 筛选 ∩ `/filter` 结果的全部)还是需要显式"全选"再确认?退出筛选视图后作用域如何界定?
3. **确认与撤销**:大批量(几十上百张)是否要两行确认(呼应 `/dedup`、退出确认的先例)?误批量后怎么整批撤销(批量 remove 是否同一入口)?
4. **与 cap 交互**:批量目标是带 cap 的标签(如"重复"或有序标签)时超 cap 怎么处理——静默截断、报数、还是拒绝整批(复用 F-18 的"只计真正打上的")?
5. **批量语义唯一性**:这套"对当前视图批量作用"要能同时覆盖打标签**和**未来可能的批量导出/批量清标签,避免每个动作各造一套选择模型——这正是 finding 要求"等手感"的核心,先想清通用形态再落第一个动作。

上述任一问题在真机使用中有了明确答案即达触发条件;在此之前不推测实现。
难度 M | 复杂度 中 | 优先级 P3

---

**F-45 Esc 消歧 20ms 阈值在高延迟终端(ssh)下会把方向键误判成 Esc**
类别: 观察记录 | 来源视角: 工程
项目环境锁定本地 Ghostty,记录即可;若未来支持远程,改读 ESC 后跟随字节的状态机(不依赖时延)。
难度 - | 复杂度 - | 优先级 P3

---

**F-46 渲染失败时 tmpfile 残留 / t=s 共享内存介质验证**
类别: 观察记录 | 来源视角: 工程
WriteFailed 分支会留下临时文件(正常路径由终端读后删除);t=s 是 M0 起挂账的可选优化。均维持既有 backlog 状态。
难度 - | 复杂度 - | 优先级 P3

---

**F-47 纯 JPEG 大项目导入无进度输出**
类别: 交互打磨 | 来源视角: 用户
逐张读 EXIF(约 2ms/张)在千张级要数秒,期间无输出。复用 ScanProgressFn 语义加一档"扫描中 X/N"(注意与 RAW 缓存进度的文案区分)。
难度 S | 复杂度 低 | 优先级 P3

---

**F-48 低频功能盘点结论(不动)**
类别: 观察记录 | 来源视角: 用户
`pzt tag list`、`r v`、`--no-prune`、cap 替换流维持现状;见 2.3 表格。此条仅留档,无行动。
难度 - | 复杂度 - | 优先级 P3

## 建议执行顺序(剩余项)

沿用原报告第七节"正常排期"里尚未做的部分(F-07 已完成、F-13 已拍板推迟已从序列中摘除):

**P2 正常排期**: 全部完成(F-16、F-22、F-20、F-24、F-14/F-15,见文末"已完成(2026-07 P2 收尾批次)")。除已拍板推迟的 F-13 外,P2 无剩余项。

**已拍板推迟**: F-13(有界解码),等真实 24MP+/60MP 素材量出数据再定上限。

**P3 Wave 1(无需触发的低风险项)**: 全部完成(F-39/F-40、F-35/F-42、F-34/F-36,见文末"已完成"节的 Wave 1)。F-36 第五子项(key-to-render 计时起点)拍板不做、留档。

**P3 剩余,按性质分三类**(不主动排期,触发条件满足或拍板后再动):
* **测量触发**: F-37(编译时长)、F-38(每帧连接开销)——**2026-07-19 Wave 2 已实测,均维持观察不动**(F-37 api.h 头解析 ~0.66s/TU×8,clean cli 全量 8.6s;F-38 open_default 184µs、每帧 ~0.9ms 占预算 1.5%)。数字与触发阈值见各条目正文,越阈值再动。
* **已拍板,无待处理项**: F-44(筛选视图批量打标签)2026-07-19 拍板维持推迟、触发前提已写清(见逐项清单 F-44);F-28、F-41 拍板为已知偏离(见"已完成"节)。P3 已无"等一句话拍板"的悬空项。
* **留档观察/用出需要再做**: F-27/F-29/F-30/F-43/F-47(交互打磨)、F-45/F-46/F-48 及 F-36 残留子项(观察记录,不动)。

## 汇总矩阵(未完成)

| 编号 | 一句话 | 类别 | 来源视角 | 难度 | 复杂度 | 优先级 | 当前状态 |
|---|---|---|---|---|---|---|---|
| F-13 | 有界解码(内存/60MP/dedup 提速) | 性能/内存 | 工程+架构+产品 | M | 中 | P2 | 已拍板推迟 |
| F-27 | 未标记剩余计数 | 交互打磨 | 用户 | S | 低 | P3 | 未实现 |
| F-29 | unarchive | 需求proposal | 用户 | S | 低 | P3 | 未实现 |
| F-30 | version 应用内改名缺口 | 交互+观察 | 用户 | S | 低 | P3 | 未实现 |
| F-36 | 残留:key-to-render 计时起点(其余四项已完成) | 观察记录 | 工程 | - | - | P3 | 拍板不做 |
| F-37 | api.h 头部重量 | 纯重构+观察 | 架构 | M | 低 | P3 | 已实测(0.66s/TU×8),维持观察 |
| F-38 | 门面连接开销重测 | 观察+性能 | 架构 | - | 低 | P3 | 已实测(~0.9ms/帧),维持观察 |
| F-43 | 导出路径记忆 | 交互打磨 | 用户 | S | 低 | P3 | 未实现 |
| F-44 | 筛选视图批量打标签 | 需求proposal | 用户 | M | 中 | P3 | 推迟(触发前提已明确) |
| F-45 | Esc 20ms 阈值远程场景 | 观察记录 | 工程 | - | - | P3 | 未实现 |
| F-46 | tmpfile 残留 / t=s 验证 | 观察记录 | 工程 | - | - | P3 | 未实现 |
| F-47 | 纯 JPEG 导入进度 | 交互打磨 | 用户 | S | 低 | P3 | 未实现 |
| F-48 | 低频功能盘点(维持现状) | 观察记录 | 用户 | - | - | P3 | 未实现 |

## 已完成(2026-07 P2 收尾批次)

原 Fix-it Night 的执行落地(F-01 到 F-33 的已完成部分)归档在 `docs/history/Fix_It_Night_2026-07_Completion_Report.md`,时间线锁在 2026-07-11/12。本节记录**那之后**从本活跃 backlog 里按"建议执行顺序"继续收口的条目,不并入历史夜的报告(避免污染那次的时间线叙述);完成后从上面的未完成清单/矩阵/执行顺序里摘除。

**F-16 抽 core/media(2026-07-19 完成)** — 新建 `core/media/media.{h,cpp}`(命名空间 `pzt::core::media`),把此前散在 `core/api.cpp`、`core/dedup/dedup.cpp`、`core/ai/evaluation_worker.cpp`、`core/project/project.cpp` 的三段 RAW 预览逻辑收成单一来源:`is_raw_path()`(权威 `kRawExtensions` 集合迁入,作为"未来加 CR2/NEF"的唯一扩展点)、`decode_preview_file()`(按扩展名分发 JPEG/LibRaw 内嵌预览)、`resolve_preview_path()`(缓存优先/兜底拼路径)。四处调用方改转调,`core/ai/evaluation_worker.cpp` 去掉对门面 `#include "core/api.h"` 的反向依赖(违分层已消除)。新增 `core/tests/media_test.cpp`(is_raw_path 大小写/扩展名边界、resolve_preview_path 三分支);行为不变,`core_tests`/`cli_tests` 两套构建(Debug + Release)全绿。全接管口径(含 project.cpp)由用户拍板。

**F-22 控制台/文本纯函数抽库补测(2026-07-19 完成)** — 把 `cli/commands/browse.cpp` 匿名空间的 `wrap_tokens`/`split_console_command`/`take_scope_token` 三个纯函数移入 `cli/text/text.{h,cpp}`(namespace `pzt::cli::text`),函数体逐字不变;browse.cpp 顶部已有 `using namespace pzt::cli::text;`,调用点无需改动。选并入 cli/text 而非新建 cli/console 库(三者不依赖 i18n,`cli_tests` 已链接 `cli_text`,无新 CMake target;由用户拍板)。`cli/tests/text_test.cpp` 追加 doctest:命令分割(无空格/多空格边界)、`take_scope_token` 引号闭合/未闭合/CJK 带空格标签/前导空格、`wrap_tokens` token 边界换行与 CJK 宽度。行为不变,Debug + Release 双构建 `cli_tests` 全绿。browse.cpp 抽帧渲染函数仍留 backlog(本条不含)。

**F-20 终端 resize 残影(2026-07-19 完成)** — `cmd_open` 主循环加"上一帧渲染尺寸"状态(`last_cols/rows/cell_px_w/cell_px_h`),帧顶跟当前 `get_terminal_size()` 比对:变化时 `write_stdout("\x1b[2J")` 整屏清除,并把 `size_changed` 并入图片渲染门(`navigated || style_toggled || size_changed`)强制按新尺寸重传图片(current_id 不变时 navigated 为假,单靠导航检测触发不了)。输入循环由用户拍板改成**主动 poll 自动恢复**:纯浏览态(不开 debug、无 AI)以前是纯阻塞 read,resize 无按键永远察觉不到;现在也带 250ms 超时 poll,超时后只有终端尺寸相对上一帧变了才 break 去重画(复用 `timed_out`→`suppress_latency_log` 路径,不打 key-to-render 日志),尺寸没变继续等、不无谓刷新。resize 后无需按键即在 ~250ms 内自动恢复、无残影。改动仅 `cli/commands/browse.cpp`;交互主循环无可单测纯函数,双构建零警告、`ctest` 回归全绿,resize 行为由真机手测(Ghostty+tmux)确认。

**F-24 会话续点(2026-07-19 完成)** — `projects` 加可空 `last_image_id` 列(`ensure_column` 迁移,旧库落 NULL = 无续点)。`ProjectSummary` 加 `last_image_id` 字段,`get_project_summary` 的 SELECT 读回;新增 core `project::set_last_image_id` + 门面 `pzt::core::set_last_image_id`(纯 UPDATE,headless-safe)。`cmd_open`:起点选择先看存的 `last_image_id` 是否仍在当前图片列表,在就从它起步、否则(从没浏览过/图已被删/prune)静默落第一张;退出时(主循环结束后,q/EOF 任一路径)写一次 `current_id`——**退出时写一次**由用户拍板(不每键写,守零延迟),**失效静默回退第一张**由用户拍板。`core/tests/project_test.cpp` 加一例:默认 nullopt、set 后 `open_project` 读回(顺带覆盖迁移列读写)。cli 起点/回退是主循环内接线,无独立可测纯函数,靠真机手测覆盖。双构建零警告、`ctest` 回归 + 新用例全绿。

**F-14/F-15 prefetch 持锁拷贝整图 + 每键全项目路径表(2026-07-19 完成)** — F-14：`PrefetchCache::Entry` 的像素改存 `std::shared_ptr<const decode::DecodedImage>`,`get()` 返回 shared_ptr(引用计数 +1)而不是持 `mu_` 期间按值拷贝整张 24MP≈96MB 像素(拷贝时长内 worker 全阻塞);worker 解码完 `make_shared` 存入,调用方 browse.cpp 改 `const auto& img = *decoded.value();`(shared_ptr 在渲染块内存活、pointee 有效),`prefetch_test.cpp` 改 `.value()->width`。F-15：`window_priority_order` 改为返回窗口内 `ImageRef*`,`set_current` 不再对全项目每张图拼绝对路径(O(n) 字符串分配、每键都跑),只为窗口内 2w+1 张里新加入的解析路径,且路径分发复用 F-16 的 `media::resolve_preview_path`(单一来源,不再是 prefetch 本地第 4 份拷贝)。延迟敏感路径的 get wait_ms / decode 日志保持不变。双构建零警告、`ctest`(prefetch 17 例 80 断言)全绿。F-13(有界解码)仍拍板推迟,与本两项无依赖。

### Wave 1(无需触发的 P3 低风险收口批,2026-07-19 起)

P3 桶里一批**不依赖数据/需求触发**的低风险项(S、行为明确、无需产品拍板),按"F-39 领头(实质补 Dedup PRD 确定性 NFR 字面)"的顺序批量收口。分三个提交:①F-39+F-40(dedup) ②F-35+F-42(终端文本) ③F-34+F-36(raw alpha + 工程规范杂项)。

**F-39 + F-40 dedup 确定性 + IN 分块(2026-07-19 完成)** — F-39:`find_duplicates_impl` 里 `groups_by_root`(`unordered_map`)的遍历序以前直接灌进 `result`,`DuplicateGroup` 顺序跨进程运行不稳定,违反 Dedup PRD 确定性 NFR 字面(打标签集合本身一致,但输出顺序不定)。改成每个候选簇内先把该簇的组收进局部 vector、按组内最小 id(`image_ids` 已升序,即 front)排序后再 append;cluster 本身按 captured_at 有序,整体输出即完全确定。F-40:`load_metas` 单条 IN 绑定全部 id,3 万+ 项目会超 `SQLITE_MAX_VARIABLE_NUMBER`;套用 `tagging::images_with_tag` 已验证的 `kChunkSize=500` 分块惯例,逐块查询累积、最后统一按 captured_at 排序。新增 `dedup_test.cpp` 一例:同一时间簇内造两组(不同哈希、组间距离超阈值不合并),断言返回的组按组内最小 id 升序(不依赖 id 实际分配顺序)。F-40 分块机械套用已验证 helper,靠现有 dedup 用例回归。双构建零警告、`ctest` 全绿。

**F-35 + F-42 终端文本渲染(2026-07-19 完成)** — F-35:`cli/text/text.cpp` 的 `is_wide_codepoint` 加 emoji 宽区间 `0x1F300-0x1FAFF`,文件名带 emoji 时按 2 列宽渲染、边框不再错位;`text_test.cpp` 现有用例补 😀/📷 及区间上下边界为宽、紧邻区间外(0x1FB00)仍为窄。F-42:`cli/ui/ui.cpp` 的 `read_text_line`(无 placeholder 版)以前单行 pad,长导出路径输入时光标越出右边框;改成与 `read_text_line_with_placeholder` 同构的两行换行(内容 = 常驻 prompt + buffer,按 content_cols 切 line1/line2、各 pad,光标字节位 `prompt.size()+cursor` 分两行定位;进入时先清 banner_row+1)。两函数本就共享 `read_line_edit_step` 状态机,只统一 redraw。F-42 是交互 redraw、无独立可测纯函数,靠真机手测(`e`/`g e` 输入超宽路径确认换行不越框);F-35 单测覆盖。双构建零警告、`ctest` 全绿。

**F-34 + F-36 raw alpha + 工程规范杂项(2026-07-19 完成)** — F-34:`core/raw/raw.cpp` LibRaw f=24 输出补的 RGBA 第 4 字节 `0`→`255`(不透明),防御未来 f=32 直连时全透明整图消失(现渲染路径不读 alpha,纯防御)。F-36 打包的**四项**落地:①`cli/ui/ui.cpp` `write_stdout` 改成循环写到全部字节落地、EINTR 重试(以前一次 `write` 拿多少算多少,短写/被信号打断会静默截断转义序列或图片数据);②`cli/commands/browse.cpp` 两处 `usleep`→`std::this_thread::sleep_for`;③退出汇总 p95 改最近秩法 `ceil(0.95*n)-1`(以前直接截断 `0.95*n`,小样本偏低);④`core/dedup/dedup.{h,cpp}` `compute_dhash` 返回 `std::optional<ImageHash>`,resize 失败(现路径不可达)返 nullopt、调用方跟解码失败同路径跳过,不再伪造合法哈希 0 参与分组;`dedup_test.cpp` 精确 bit pattern 断言改 `.value()`。F-36 的**第五项(key-to-render 计时起点挪到 read() 后)拍板不做**——见上"逐项清单/F-36 残留子项"的完整理由(导航键收益微秒级,而挪起点会把 x/submit 的故意 UI 停顿折进延迟指标、失真汇总)。F-34/write_stdout/usleep/p95 无可注入的独立测试点(交互或真实 RAW 路径),靠双构建 + 回归 + 真机手测;compute_dhash 单测覆盖。双构建零警告、`ctest` 全绿。

**Wave 1 收口结论**:F-39/F-40、F-35/F-42、F-34/F-36 三个提交完成,P3 里"无需触发的低风险项"清空。P3 剩余项按性质分三类留待后续:**测量触发**(F-37 编译时长、F-38 每帧连接开销,需先出数字再决定动不动)、**已拍板**(2026-07-19:F-28 有序标签调序、F-41 pzt list 计数记为已知偏离;F-44 筛选视图批量打标签维持推迟、触发前提写清)、**留档观察不动**(F-45/F-46/F-48,以及 F-36 残留的计时子项)。其余交互打磨(F-27/F-29/F-30/F-43/F-47)维持"用出需要再做"。

**F-28 有序标签调序 UI(2026-07-19 拍板:文档化为已知边界,不实现)** — 有序标签(`is_ordered=true`,用户在建标签流程里 y/n 选,见 `cli/menu/tag_menu.cpp`)的 `position` 语义已完整:打标签时按 `MAX(position)+1` 分配、导出按位编号、`replace_tag_entry` 新图继承旧图 position。缺口是**顺序永远 = 打标签顺序,没有事后调序操作**(想让某张变成第 1 位只能删掉重打)。用户拍板取分支 (b):**明确"有序标签的顺序 = 打标签顺序",降级为已知边界,不做筛选视图内的上移/下移**。理由:调序是 M 工量(全剩余 backlog 里最大单项),而暂无真实调序需求信号,跟 F-44 同一道纪律——不推测性建 M 型交互,留门给真实需求出现时再做分支 (a)。**零代码改动**。archived Eng Design(position 语义的权威出处)不回改,以本条拍板为准。

**F-41 pzt list 各标签计数(2026-07-19 拍板:记为已知偏离,不实现)** — M0 PRD 字面承诺 `pzt list` 展示"各标签下的图片数量",现状只展示项目名 + 图片总数 + 根路径 + 归档态。用户拍板**保持 `pzt list` 轻量、不实现 per-tag 聚合**:per-tag 计数已由 `pzt tag list <项目>`(commands.cpp `tag_list` → `list_tags` 返回 `TagSummary.tagged_count`)和应用内 space 菜单承载,`list` 再展开会让多标签项目每行拖一串计数、且与既有计数逻辑重复。**零代码改动**。记录位置:本条 + `docs/SPEC.md` 3.1 节 `list` 命令处加了接口轮廓说明;**archived M0 PRD(docs/history/)不回改,避免改写已完成里程碑的历史文本**——倒查 M0 PRD 看到该承诺时,以此处拍板为准。
