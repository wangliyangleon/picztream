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

**F-28 有序标签没有调序 UI(九宫格故事只兑现一半)**
类别: 需求proposal + 观察记录 | 来源视角: 产品
position 语义存在(replace 继位、导出按位编号)但顺序只能等于打标签顺序。分支: (a) 筛选视图内加"上移/下移"操作(M);(b) 明确文档化"顺序 = 打标签顺序",降级为已知边界。先拍板,不默认实现。
难度 M | 复杂度 中 | 优先级 P3

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

**F-34 raw 解码 RGBA 第 4 字节填 0 改 255**
类别: 工程规范 | 来源视角: 工程
见 4.4。防御 f=32(alpha 有效)路径未来直连时的全透明陷阱。
难度 S | 复杂度 低 | 优先级 P3

---

**F-36 工程规范杂项打包**
类别: 工程规范 | 来源视角: 工程
一次顺手修完: write_stdout 短写循环;usleep 换 sleep_for;key-to-render 计时起点挪到 read() 后(现在漏掉按键处理与 set_current);p95 索引小样本偏差;compute_dhash 失败返回 0 改 optional(现路径不可达,纯防御)。
难度 S | 复杂度 低 | 优先级 P3

---

**F-37 api.h 头部重量: nlohmann/thread 透传给全部 cli TU**
类别: 纯重构 + 观察记录 | 来源视角: 架构
api.h -> evaluation_worker.h -> ai.h -> nlohmann/json.hpp,i18n.h 又 include api.h,全 cli 编译单元都在解析 json 头。本周(W2026-07-15)agent Style/curate 落地后 api.h 又新增了 `core/curate/curate.h`、`core/settings/settings.h` 等重导出(见 core/api.h 顶部 include 列表),头只增不减、观察结论不变。工程规模尚小,等编译时间成为体感问题再动(方案: worker pimpl 或 api.h 拆分)。
难度 M | 复杂度 低 | 优先级 P3

---

**F-38 门面每帧多次开连接: 重测数字,维持观察**
类别: 观察记录 + 性能/内存 | 来源视角: 架构
Optimization_Backlog 既有条目。schema init 变厚后建议重测一次每帧真实开销(RelWithDebInfo 下 key-to-render 是否仍在 60ms 档);热点已由 F-07 单独消灭。注:开库语句数本周又有变动——recipe 扩展新增 5 条 `ensure_column`(grain_amount/contrast/saturation/blacks/whites),F-33 删掉 4 条 `ensure_column_dropped`,重测时以当前 schema 为准。若 M4 headless 层立项,顺势引入持连接的 core::Session,现在不预做。
难度 -(测量) | 复杂度 低 | 优先级 P3

---

**F-41 pzt list 缺各标签计数(M0 PRD 字面承诺)**
类别: 文档修正 或 Bug | 来源视角: 产品
二选一: 实现(list_projects 加聚合,S)或修订 PRD 措辞。倾向后者(应用内 space 菜单已带计数,list 保持轻)。
难度 S | 复杂度 低 | 优先级 P3

---

**F-43 导出路径记忆**
类别: 交互打磨 | 来源视角: 用户
`e`/`g e` 的路径输入预填上一次用过的目录(会话内存级即可,持久化并入 F-12 配置)。
难度 S | 复杂度 低 | 优先级 P3

---

**F-44 对当前筛选视图批量打标签**
类别: 需求proposal | 来源视角: 用户
"筛出不达标 -> 全部打废片"这类批量动作的通用形态。F-09 的 `/filter` 二级筛选已落地(见完成报告),提供了"筛出"这一半;"对筛选结果批量打标签"这另一半仍未做,等 `/filter` 用出手感后一并设计,避免两套批量语义。
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

**留档观察**: P3 其余条目,按"真实需求/数据触发"原则处理,不主动排期。

## 汇总矩阵(未完成)

| 编号 | 一句话 | 类别 | 来源视角 | 难度 | 复杂度 | 优先级 | 当前状态 |
|---|---|---|---|---|---|---|---|
| F-13 | 有界解码(内存/60MP/dedup 提速) | 性能/内存 | 工程+架构+产品 | M | 中 | P2 | 已拍板推迟 |
| F-27 | 未标记剩余计数 | 交互打磨 | 用户 | S | 低 | P3 | 未实现 |
| F-28 | 有序标签调序缺口(先拍板) | 需求+观察 | 产品 | M | 中 | P3 | 未实现 |
| F-29 | unarchive | 需求proposal | 用户 | S | 低 | P3 | 未实现 |
| F-30 | version 应用内改名缺口 | 交互+观察 | 用户 | S | 低 | P3 | 未实现 |
| F-34 | raw alpha 字节 0 改 255 | 工程规范 | 工程 | S | 低 | P3 | 未实现 |
| F-36 | 规范杂项包(短写/usleep/计时/p95等) | 工程规范 | 工程 | S | 低 | P3 | 未实现 |
| F-37 | api.h 头部重量 | 纯重构+观察 | 架构 | M | 低 | P3 | 未实现 |
| F-38 | 门面连接开销重测(维持观察) | 观察+性能 | 架构 | - | 低 | P3 | 未实现 |
| F-41 | pzt list 标签计数 drift 收口 | 文档修正 | 产品 | S | 低 | P3 | 未实现 |
| F-43 | 导出路径记忆 | 交互打磨 | 用户 | S | 低 | P3 | 未实现 |
| F-44 | 筛选视图批量打标签 | 需求proposal | 用户 | M | 中 | P3 | 未实现 |
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
