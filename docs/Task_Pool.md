# PZT 中长期任务池(低优先级 backlog)

> 这是"闲时拿一个来修"的活儿池。**这里的条目默认都是低优先级**——没有短期必须做的,不标 priority;真正紧急/近期必修的东西不会进这里,会走当周 PRD 或直接开工。
>
> 每条只标**预估 size**(S=半天内 / M=1-3 天 / L=3 天以上或需先出设计文档)和**依赖/触发前提**(有则列,决定"现在能不能直接开工")。想动一条之前先看它的依赖是否满足。
>
> 编号多沿用 2026-07 Fix-it Night 评审(F-XX);非 Fix-it 来源的条目(如从某周开发目标顺延下来的)用描述性名字并在依赖列标注来源。每条的完整背景、file:line 证据、四视角分析在归档报告里:`docs/history/Fix_It_Night_Review.md`(未完成快照 + 拍板记录)与 `docs/history/Fix_It_Night_2026-07_Completion_Report.md`(原始评审分析)。**已拍板终结的条目(F-28 有序标签调序、F-41 pzt list 计数、F-36 残留计时子项)不在本池**,它们是"已知边界/不做"的决定,只在归档报告留档。
>
> 新起一条前:确认它没被后来的代码改动淘汰(证据可能漂移),按分层契约(`core`/`cli`/`agent`)判断归属,涉及行为语义变化的先出 PRD 再动(见 `CLAUDE.md`)。

## 可直接开工(无外部依赖)

这些 size 小、依赖为空,是最适合"闲时顺手清一个"的:

| 编号 | 一句话 | Size | 依赖/触发前提 | 备注 |
|---|---|---|---|---|
| F-29 | 补 `unarchive` 子命令 | S | 无 | M0 起挂账的对称缺口。core 一个 UPDATE(清 `archived_at`) + cli 一个子命令,对称 `archive`。 |
| F-27 | 信息栏显示"未标记剩余 N 张" | S | 无 | j/k 工作流的进度感。一条 COUNT 查询 + 一行展示。**别做成每帧全表扫描**(信息栏每帧重画;计数走增量或缓存,跟 F-38 的每帧开销一起权衡)。 |
| F-47 | 纯 JPEG 大项目导入进度输出 | S | 无 | 逐张读 EXIF(~2ms/张)在千张级要数秒、期间无输出。复用 `ScanProgressFn` 加一档"扫描中 X/N"(与 RAW 缓存进度文案区分)。 |
| F-43 | 导出路径记忆 | S | 会话内存级无依赖;持久化则接入已有 Settings | `e`/`g e` 路径输入预填上次用过的目录。会话内存级即可独立做(S);要跨会话持久化就存进配置(原 F-12 Settings 已落地)。 |
| F-30 | version 改名的应用内入口 | S | 触发:真实使用出现改名需求 | 现在只有 CLI 寻址语法能改 version 名,应用内无入口。低频;真用到了再决定加 `r` 菜单 rename 还是维持现状。 |
| F-46a | 渲染失败时 tmpfile 残留清理 | S | 无 | Kitty `WriteFailed` 分支会留下临时文件(正常路径由终端读后删除)。补一个失败路径的显式清理。 |
| Origin 彩蛋命名 | `r` 菜单里 `0:[Origin]` 的显示名换成"系统 locale 城市 + 当前年份"(如 "Shanghai 2026"),跟 9 个 City+Year 预设(`core/recipe/recipe.cpp::builtin_presets`)的命名语感呼应 | S | 无,纯展示彩蛋;行为不变(仍是 `recipe_id=NULL` 清除风格),全图统一一个名字,不按单张照片的拍摄地。开工时需定"城市"取值来源(系统 locale 区域码本身只到国家/地区,没有城市颗粒度,要么接 `CoreLocation` 反查真实定位,要么退化成一份区域码→代表城市的静态映射表)。 |

## 有依赖 / 需先满足触发前提

| 编号 | 一句话 | Size | 依赖/触发前提 |
|---|---|---|---|
| bottle 分发 + 安装统计 | CI 构建预编译 bottle 作为 GitHub Release asset —— 装得更快(不用每次源码构建)+ 拿到真实安装数(读 asset `download_count`) | M | **触发:有一定用户量、想要精确安装曲线,或嫌源码构建慢。** 现状:自托管第三方 tap 是 source-build,Homebrew 官方 analytics 不覆盖它,只能靠 tap 仓 Insights→Traffic 的 clone 流量粗估(14 天窗口、只知"tap 过"不知装了哪个)。做法:release workflow 里加按 macOS 版本 `brew bottle` 产出、传成 Release asset、formula 加 `bottle do` block。见 `docs/RELEASE.md`。 |
| 几何变换 | 裁切 + 水平矫正,走 `set_image_recipe` 同一条路径应用并渲染(非另起一套机制);也是 agent `Style` Stage 的前置阻塞项之一 | L | **先出 Eng Design**。产品要求已在 `docs/W2026-07-15_PRD.md` 目标二第 3 项落定(验收标准都写好了),缺的是工程方案——裁切改图像尺寸、矫正需旋转重采样,牵涉渲染管线、预取缓存与 version schema。本周目标二拆两刀实现时未纳入、顺延至此。背景见 `docs/history/W2026-07-15_RecipeExpansion_Eng_Design.md`(归档说明)与 `docs/SPEC.md` 未来/搁置一节。 |
| F-44 | 对当前筛选视图批量打标签 | M | **先用出 `/filter` 手感 + 先出 PRD**。触发前提是能回答 5 个设计问题(入口与触发、作用域边界、确认与撤销、与 cap 交互、批量语义唯一性)——细节见归档报告 F-44 条。核心是"对当前视图批量作用"要能同时覆盖打标签/未来批量导出/清标签,避免两套选择模型。 |
| F-13 | 有界解码(subsampled decode) | M | **先在真实 24MP+/60MP 素材上量 RSS 与 60MP key-to-render**,把收益写成数字再定 max_px 上限 | 预取缓存全分辨率 RGBA(24MP×7≈0.7GB;60MP 单张解码~180ms 超预算)、dedup 全量解码(~70ms/张)、AI 上传同源的统一杠杆。方案:`core/decode` 加 `decode_jpeg_file_bounded(path, max_px)`(`CGImageSourceCreateThumbnailAtIndex`)。当前 24MP 内体验尚达标,不算急。 |
| F-37 | api.h 头部重量(json 头透传全 cli TU) | M | 触发:cli 编译时长成体感(header-cascade 影响 edit-build 循环) | 已实测基线(2026-07-19):api.h 头解析 ~0.66s/TU,8/13 cli TU 透传,clean cli 全量 8.6s——**未越阈值**。越阈值再动:worker pimpl 让 `ai.h`/`json.hpp` 停止透传,可从 8 个 TU 各砍 ~0.66s。 |
| F-38 | 门面每帧多次开连接 | M | 建议随 M4 headless 层立项一起做(引 `core::Session` 持连接);否则触发=key-to-render 超 60ms | 已实测基线(2026-07-19):`open_default`(开连接+全量 schema init)~184µs,导航帧 ~5 次 O(1) 开连接 ≈0.9ms/帧、占 60ms 预算 1.5%——**可忽略**。不预做。 |
| F-45 | Esc 消歧 20ms 阈值在高延迟终端误判方向键 | M | 依赖:远程/ssh 支持立项(当前锁定本地 Ghostty,不触发) | 若未来支持远程,改读 ESC 后跟随字节的状态机(不依赖时延),而不是靠 20ms 时间窗。 |
| Apple Vision 聚类评估 | 评估 `VNGenerateImageFeaturePrintRequest` 语义聚类相对现有 dHash+时间窗口的优劣,决定要不要替换 dedup/curate 的分簇底层 | L | **先出 Eng Design(含新依赖评估)**。来源:`docs/W2026-07-21_PRD.md` 拍板——本周锦标赛改造复用现有 dHash,Apple Vision 语义聚类(更懂"同一场景"而非仅"近乎相同")单列于此。需新增 Vision.framework + ObjC++ 桥接;先在真实素材上比 featureprint 距离 vs dHash 汉明距离的分簇质量,把收益写成数字再决定是否切换。 |
| F-46b | Kitty `t=s` 共享内存介质验证 | S | 无(可选优化,需真机验证收益) | M0 起挂账的可选传输优化,收益需在真机量过再决定是否切换。 |

## 留观察,暂不列为活儿

* **F-48 低频功能盘点结论**:`pzt tag list`、`r v`、`--no-prune`、cap 替换流经评审后**维持现状、无行动**。仅记录,不是待办。

---

**维护约定**:开工一条前把它从"可直接开工/有依赖"挪到进行中,完成后连同一句话结论移除或标完成;若某条被后来的改动淘汰(证据漂移、需求消失),直接删并在 commit 说明。本池只装"可能会拿来修"的活儿,拍板终结的决定回归档报告,不在这里堆积。
