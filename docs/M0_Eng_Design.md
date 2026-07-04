# PicZTream (PZT) Milestone 0 工程设计文档

## 背景

`docs/M0_PRD.md` 明确把 SQLite 表结构留待"下一步单独设计"；`AGENTS.md` 的工程契约要求具体的模块划分、schema 与 tradeoff 必须先落到对应里程碑的 eng design 文档,不允许 agent 在没有文档的情况下臆测细节自行实现。本文档就是 M0 缺失的那一份 eng design,implementation 应在本文档评审通过后再开始。

## 渲染延迟验证结论

对应 M0_PRD.md 风险清单里"Kitty 图像协议在 Tmux 窗格内的实际渲染延迟目前尚未经过实测"这一条,已经用一个独立探针(`spikes/kitty_latency_probe/`)做了验证,完整数据见该目录下的 `results.md`,这里只摘要对设计有直接影响的结论:

1. **JPEG 不能直接喂给 Kitty 协议**。协议的 `f=` 格式字段只支持 RGB/RGBA/PNG 三种像素载荷,没有 JPEG。Roadmap.md 里"绕过 RAW 解码、直接投喂"的表述,在终端渲染这一步并不是字面意义的零解码——真正绕过的是 RAW 的 Bayer/X-Trans 去马赛克(M2 范畴),JPEG→像素的解码这一步在 M0 是无法省略的,必须纳入延迟预算。
2. **解码成本随像素数近似线性增长**(实测约 3ms/MP),这印证了 M0_PRD.md 已有的预取设计是对的方向:解码必须发生在异步预取阶段,不能算进按键触发的同步延迟预算。
3. **传输介质的选择比想象中更关键**。把编码好的像素通过 base64 经 pty 转义序列直接发送(`t=d`)在 24MP 档就已经要 900ms,完全不可用;改用 Kitty 协议的文件传输介质(`t=t`,转义序列里只带一个 base64 编码的文件路径,由终端自己读盘)能把"发送"这一步的成本从几百毫秒压到个位数微秒的控制序列 + 一次磁盘写入。
4. **100ms 目标在预取命中的前提下,对中等像素素材是可以达到的,但对 60MP+ 级素材大概率达不到**:解码+`t=t`传输合计,12MP 档约 47ms、24MP 档约 93ms 都在预算内,60MP 档约 330ms 明显超标。这意味着 M0_PRD.md 风险清单里预留的"降级到较低分辨率预览图"兜底方案,不是要不要做的问题,而是大概率需要在遇到高像素素材时触发的真实路径。
5. **环境前提**:tmux 的 `allow-passthrough` 选项本机默认是关闭的,Kitty 图形转义序列要从 tmux 窗格穿透到 Ghostty 必须显式 `set -g allow-passthrough on`,否则图完全不会渲染。这是一个此前任何文档都没提到的启动前提。
6. **未验证部分**:探针本次经由非交互式管道执行,拿到的是解码与磁盘 I/O的可信数据,但真实 pty 的 write 行为、终端 ACK 往返、以及肉眼可视确认还没有采集,需要后续在真实交互式 Ghostty+Tmux 会话里手动跑一遍探针做最终确认。
7. **色彩管理缺口**:探针的解码步骤用 `CGBitmapContext` + device RGB 直接取像素,没有读取或应用源 JPEG 内嵌的 ICC 色彩描述文件(如 Adobe RGB、Display P3)。Kitty 协议的像素载荷本身不带色彩配置信息,任何色彩空间转换都必须在传输前完成。对一个以照片筛选为核心场景的工具,这意味着广色域素材在当前实现下大概率会有可感知的色偏,即使 M0 明确不做色彩处理/风格预设(M1 范畴),"如实还原 JPEG 本身编码的颜色"和"应用风格化色彩处理"是两件不同的事,前者更接近解码正确性问题而不是 M1 范畴的功能。

### 对 core 设计的直接影响

- 预取/缓存模块的职责必须包含"JPEG→像素解码"这一步,且必须在异步线程完成,不能出现在浏览导航的同步路径上
- cli 渲染模块应该优先使用 `t=t`(或后续验证更优的 `t=s` 共享内存介质)而不是 `t=d` 直接传输
- 需要一个针对超高像素(实测 60MP 档超标)素材的降采样预览兜底路径,具体降采样策略留给 cli 实现阶段决定,但预取模块的接口需要预留这个分支
- cli 需要同时支持"运行在 Tmux 窗格内"与"直接运行在独立 Ghostty 窗口内(不经过 Tmux)"两种模式:启动时通过 `$TMUX` 环境变量探测当前处于哪种模式,只有探测到 Tmux 时才需要走 DCS passthrough 包装(`\x1bPtmux;...\x1b\\`,并检测 `allow-passthrough` 是否开启、不满足则给出明确提示),独立 Ghostty 窗口下转义序列直接发送,不做包装,也不需要检测 `allow-passthrough`。这个探测与分支逻辑属于终端渲染细节,只能放在 `cli`,不能下沉进 `core`

## 数据库 Schema 设计

四张表,对应 M0_PRD.md 技术方案概要里点名的四张表。数量上限的校验放在 core 的业务逻辑层做,不放进数据库约束——SQLite 没有方便的方式声明式表达"某个分组的计数不能超过某个值"这种跨行约束。

```
projects(
  id            INTEGER PRIMARY KEY,
  name          TEXT NOT NULL UNIQUE,
  root_path     TEXT NOT NULL,
  created_at    INTEGER NOT NULL,  -- unix 时间戳
  archived_at   INTEGER            -- NULL 表示未归档,非空即归档时间戳
)

images(
  id            INTEGER PRIMARY KEY,
  project_id    INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
  file_path     TEXT NOT NULL,   -- 相对 root_path 的相对路径
  file_name     TEXT NOT NULL,
  file_size     INTEGER NOT NULL,
  imported_at   INTEGER NOT NULL,
  UNIQUE(project_id, file_path)
)

tags(
  id            INTEGER PRIMARY KEY,
  project_id    INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
  name          TEXT NOT NULL,
  cap           INTEGER,         -- NULL 表示不限数量
  is_ordered    INTEGER NOT NULL DEFAULT 0,  -- 布尔,是否关心组内顺序
  is_system     INTEGER NOT NULL DEFAULT 0,  -- 布尔,标记内置的"废片"标签
  UNIQUE(project_id, name)
)

image_tags(
  image_id    INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
  tag_id      INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  position    INTEGER,           -- 仅当对应 tag.is_ordered 时有意义
  tagged_at   INTEGER NOT NULL,
  PRIMARY KEY (image_id, tag_id)
)
```

补充说明:

- `is_system` 用来标记 M0_PRD.md 键位设计里 `x`(废片)这个内置标签,和用户自定义标签共用同一张表,区别只在于它由系统在建项目时自动创建、不可删除
- `images.file_path` 存相对路径而不是绝对路径,是为了让项目文件夹被移动后依然可以正确解析(M0 虽然不做多设备同步,但本地移动文件夹是常见操作)
- `image_tags.position` 只有在 `tags.is_ordered = 1` 时才由业务逻辑维护,未排序标签下这一列始终为 NULL
- `archived_at` 只影响 `list` 的展示分组,不影响任何查询/浏览行为——归档不是锁定,归档后的项目仍然可以正常 `open`
- 三张子表的外键都带 `ON DELETE CASCADE`(需要 `PRAGMA foreign_keys = ON`),配合这一点,删除项目时只需要 `DELETE FROM projects WHERE id = ?` 一条语句,`images`/`tags`/`image_tags` 会自动级联清除,不需要手写多条 DELETE,也不会留下孤儿数据。磁盘上的原始图片文件不受影响,这是纯粹的元数据清除

## core/api 接口设计

`cli` 只能通过下面这层门面调用 `core`,不允许绕过它直接碰数据层。

**项目**
- `create_project(name, folder_path) -> ProjectId` — 扫描文件夹内的 JPEG,写入 `images`;找不到任何 JPEG 时返回错误,不创建项目。`folder_path` 由 cli 层负责传入(`pzt new` 省略 `folder_path` 参数时 cli 默认取当前工作目录),`core` 本身不对"当前目录"这个概念有任何认知,保持通用
- `find_project_by_name(name) -> optional<ProjectId>` — 供 `pzt open <project_name>` 带名字时直接查找
- `find_project_by_root_path(path) -> optional<ProjectId>` — 供 `pzt open` 省略 `project_name` 时按 cwd 反查项目;两种查找方式共用同一条"找不到就报错,提示用 pzt list 查看"的错误路径,这个交互细节在 cli,`core` 只做精确匹配
- `open_project(project_id) -> ProjectHandle` — 归档状态不影响能否打开;不管是按名字还是按 cwd 找到的,最终都统一走这一个入口
- `list_projects() -> vector<ProjectSummary>`(含根路径、图片总数、各标签计数、归档状态;归档项目排序规则由 cli 在渲染 `list` 时处理,`core` 只需要如实返回 `archived_at`)
- `archive_project(project_id)` — 设置 `archived_at`,纯元数据操作
- `delete_project(project_id)` — 级联清除该项目的 `projects`/`images`/`tags`/`image_tags` 四张表数据(依赖 schema 里的 `ON DELETE CASCADE`),不触碰磁盘文件
- `rescan_project(project_id, prune = true) -> Result<RescanSummary, ProjectNotFoundError>` — 补录项目建好之后新增到磁盘上、但 `images` 表里还没有的文件,复用 `create_project` 内部的扫描逻辑;`prune` 为真(默认)时,同时清除 `images` 表里有、但磁盘上已经找不到对应文件的记录,级联清掉这些图片打过的标签(`ON DELETE CASCADE`)。这是 increment 6.4 阶段实际使用中发现的修正:最初设计成只增不减,是为了防止磁盘上文件暂时缺失(比如外置硬盘没挂载)时误删记录、悄悄丢失标签,但实测下来"删掉的照片一直清不掉"比这个风险更让人困惑,因此改成默认清理,把"暂时不完整的存储位置"这种场景留给 `prune=false`(cli 侧对应 `--no-prune`)显式处理

**浏览**

"当前浏览到哪张图"不是 `core` 的状态——真正的全键盘循环(increment 6)是单次进程运行内的内存状态,调用方自己持有 `current_id` 并在每次导航后更新它,`core` 只提供无状态的查询/计算函数:

- `list_images(project_id) -> vector<ImageRef>` — 项目内所有图片按 `file_path` 字典序排列,一个确定性的浏览顺序基准
- `next_image(images, current_id) / prev_image(images, current_id)` — 纯内存运算,不查库;`current_id` 为空或者不在列表里时防御性地从头(`next`)/从尾(`prev`,循环语义)开始;到达两端时循环折返
- `next_untagged(images, current_id) / prev_untagged(images, current_id)` — 按循环顺序扫描,跳过已经打过至少一个标签的图片;扫描一整圈都没有未打标签的图片时返回空(有意义的"全部处理完了"终止信号,不会死循环)
- `filter_by_tag(tag_id) -> Result<vector<ImageRef>, BrowseTagError>`(有序标签按 `position` 升序,无序标签按 `tagged_at` 升序,跟标签模块、导出模块的排序规则保持一致)

`current_image() -> ImageHandle`(从预取环形缓冲区取已解码像素)是 increment 6 才会引入的东西,不在这次的 `core/browse` 范围内。

**标签**
- `create_tag(name, cap, is_ordered)`
- `add_tag(image_id, tag_id) -> Result<Ok, CapExceeded{existing_entries}>` — 超出上限时返回已有条目列表,由 cli 据此渲染"选一个替换"的状态栏提示;超限规则的判断在 core,替换交互的 UX 在 cli,两者不混
- `replace_tag_entry(tag_id, remove_image_id, add_image_id)`
- `remove_tag(image_id, tag_id)`

**导出**

行为流程:解析标签下所有图片(有序标签按 `image_tags.position` 升序,无序标签按 `image_tags.tagged_at` 升序,后者只是为了让多次导出行为确定、顺序本身无语义)→ 目标文件夹不存在时自动创建 → 逐张计算目标文件名、遇冲突自动消歧、按 `link_mode` 复制或软链 → 源文件在磁盘上缺失时跳过该张并记录原因,不中断其余图片的导出 → 返回汇总结果供 cli 渲染成一行状态提示。

文件命名规则:有序标签用`{序号}_{原文件名}`,序号零填充,宽度取 `max(2, 本次导出图片总数的位数)`,避免位数不齐导致文件管理器/上传工具按文件名排序时错序;无序标签直接用原文件名。命名冲突(两次导出、或不同来源子文件夹恰好同名)时在文件名末尾、扩展名之前追加 `_2`、`_3`……直到不冲突,例如 `01_IMG_1234_2.jpg`。

```
enum class LinkMode { Copy, Symlink };

struct ExportSkipped {
  ImageId image_id;
  string  reason;   // 目前只有一种取值:源文件缺失
};

struct ExportResult {
  int exported_count;
  vector<ExportSkipped> skipped;
};

ExportResult export_tag(TagId tag_id, string output_folder,
                         LinkMode link_mode = LinkMode::Copy);
```

对应 cli 子命令新增一个可选 flag:`pzt export <project_name> <tag_name> <output_folder> [--link]`,默认复制、`--link` 时软链(复制更安全,导出后与原项目文件夹解耦,更贴近"方便后续手动发布"这个用户故事)。

## 模块划分与并发模型

`core/` 内部按职责拆成五个子模块,任何一个都不允许出现终端渲染或按键交互相关的依赖:

- `core/db/`:schema、迁移、四张表的 DAO
- `core/project/`:文件夹扫描、项目注册、按名字/按路径查找项目、归档与删除、补录新增文件(`rescan_project`)
- `core/tagging/`:标签增删、cap 校验与替换逻辑、顺序语义
- `core/browse/`:导航、过滤查询、预取/缓存环形缓冲区
- `core/export/`:导出操作(复制/软链、有序编号命名、冲突消歧、缺失源文件容错)

预取/缓存用 `std::jthread` 驱动的环形缓冲区实现,围绕当前浏览索引前后各 N 张,后台线程负责"读文件字节 + JPEG 解码到像素"这一整条链路(呼应上面渲染延迟验证结论的第 2 条);缓冲区大小 N 待后续用真实素材测出合理默认值,不在本文档里预设具体数字。延迟敏感路径(浏览导航、预取)按 AGENTS.md 要求配套延迟日志,不能只靠单元测试。

**工具链前提(脚手架阶段已确认)**:本机 Apple Clang 17.0.0 / libc++ 的 `std::jthread`、`std::stop_token` 并非默认可用——头文件物理存在但被 `#if _LIBCPP_STD_VER >= 20 && !defined(_LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN)` 挡住,因为 Apple 认为这部分实现还没到 ABI 稳定,需要显式加编译期 `-fexperimental-library` 才能解锁(链接期同样要加,否则找不到对应符号)。这不是 AGENTS.md 要重新评估"是否用 jthread"的理由——`-fexperimental-library` 已经在根 `CMakeLists.txt` 里全局加上(不止 Debug 构建,Release 也需要,因为 `core/browse` 的预取线程是正式功能代码,不是调试设施),`docs/M0_PRD.md`/`AGENTS.md` 的 jthread 约束原样成立,只是多了这一条构建前提,写在这里防止以后有人看到一个"莫名其妙报缺 symbol"的构建错误却不知道原因。

## 技术选型

以下选型在此提出,待确认后作为后续实现阶段的既定前提:

- **SQLite 访问**:直接用 `sqlite3` C API,不引入额外的 C++ 封装库,保持依赖最少
- **CLI 参数解析**:手写的子命令分发(`new` / `open` / `list` / `export` / `archive` / `delete` 六个子命令,复杂度不足以引入专门的解析库),符合"不引入运行时开销较大的框架"的约束
- **单元测试框架**:引入 doctest(header-only,只作为测试目标的依赖,不进入发布二进制)

## 任务分解(Task Breakdown)

以下是本文档评审通过之后,后续开发会话要执行的工作序列,每一项标注它对应 M0_PRD.md 验收标准里的哪一条。本文档评审阶段不执行这些工作。

1. **脚手架搭建**:CMake + Ninja 构建链,从第一个 commit 起就落实 `core/` / `cli/` 分离,引入 SQLite 与 doctest 依赖
2. **`core` 实现**:量级明显大于其他步骤,实现阶段进一步拆成有序 increment,每个 increment 结束都要能实际跑 `pzt` 命令手动验证,而不是最后一起验收:
   - **increment 1(已完成)**:数据层与 schema 迁移,含 `ON DELETE CASCADE` 外键;项目导入(递归扫描当前目录/指定目录 → JPEG 发现 → 写入 `images`,目录内无 JPEG 时报错不建项目)→ 落地 `pzt new [folder_path]` / `pzt list`(对应验收标准第 1、4 条)。同时确立了 core 全局的 `Result<T,E>` 错误处理约定(`core/result.h`,不用异常表达预期业务结果)和全局 SQLite 库位置(`~/.config/pzt/pzt.db`,遵循 `$XDG_CONFIG_HOME`)
   - **increment 2(已完成)**:项目生命周期——按名字/按路径查找项目(供 `open` 的两种调用形式使用)、`open`(桩实现,只重新查询并打印项目摘要,不产生真正的浏览会话状态)、归档(`archived_at`,幂等)、删除(级联清元数据、不动磁盘文件,`pzt delete` 要求重新输入项目名二次确认)→ 落地 `pzt open [project_name]` / `pzt archive <project_name>` / `pzt delete <project_name>`(对应验收标准第 6 条)
   - **increment 3(已完成)**:标签逻辑——CRUD、cap 校验、超限替换流程、有序/无序语义(对应验收标准第 3 条)。落地为一组明确标注"临时调试用"的非交互子命令 `pzt tag create|list|add|remove|replace`,真正的交互(space 菜单、状态栏替换提示)留给 increment 6 全键盘循环。`add_tag`/`remove_tag` 都设计成幂等(重复打标/重复移除都是无害的成功,不报错);`replace_tag_entry` 让新图片接管旧图片原来的 `position`,不追加到末尾;`remove_tag` 不做 `position` 重排,靠导出阶段本来就会重新编号来兜底空位。同时给 `core/project` 补了 `ImageId`/`find_image_by_path`/`get_image` 三个小接口,`core/db/stmt.h` 把 `Stmt`/`exec_simple` 从 project 模块内部抽成两个模块共用的小工具
   - **increment 4(已完成)**:浏览与过滤——`list_images`(按 `file_path` 排序)、`next_image`/`prev_image`(纯内存运算,两端循环折返)、`next_untagged`/`prev_untagged`(循环扫描,全部打完标签时返回空、不死循环)、`filter_by_tag`(复用标签模块的排序规则)。落地为 `pzt browse next|prev|next-untagged|prev-untagged|filter` 一组临时调试子命令,风格照抄 `pzt tag`。规划阶段发现一个缺口并在这个 increment 一并解决:项目建好之后往文件夹里新增的照片本来不会被浏览到(扫描只在 `pzt new` 时发生一次),补了 `core/project::rescan_project` + `pzt rescan <project_name>`,当时设计成只增不减,不处理磁盘上消失的文件(避免悄悄丢失已打的标签)——这一点后来在 increment 6.4 阶段改成默认 `prune`,见"core/api 接口设计"一节
   - **increment 5(已完成)**:导出(含 --link、命名规则、冲突消歧,设计已在本文档"导出"小节写完,落地时补上了当时还没确定 `Result<T,E>` 约定前留下的空隙——`export_tag` 改为返回 `Result<ExportResult, ExportTagError>`,`ExportSkipped` 补了 `file_name` 字段方便 cli 打印。直接复用 increment 4 的 `browse::filter_by_tag` 拿排好序的图片列表,不重新写排序查询。落地为 `pzt export <project_name> <tag_name> <output_folder> [--link]`,对应验收标准第 5 条。规划阶段发现一个遗留问题记在"待确认问题"里:导出目标文件夹如果设在项目 `root_path` 内部,会被下次 `rescan` 当成新照片扫进去)
   - **increment 6**:预取/缓存 + 全键盘交互循环 + Kitty 渲染,对应验收标准第 2 条,是 core 实现里最大的一块,内部再拆成有序子步骤,每个子步骤结束都要能实际跑 cli 验证:
     - **6.1(已完成) JPEG 解码模块**:新增 `core/decode`,复用 `spikes/kitty_latency_probe/` 验证过的 ImageIO/CoreGraphics 解码路径(`CGImageSourceCreateWithData` → `CGImageSourceCreateImageAtIndex` → `CGBitmapContext` device RGB → `CGContextDrawImage`),吐出 `DecodedImage{width, height, rgba}`。这一步顺带收口了"待确认问题"里悬了很久的色彩管理缺口:确认相信 CoreGraphics 默认的 ColorSync 色彩匹配行为,不额外写 ICC 验证或转换代码,以后真遇到可感知色偏问题再回头处理。延迟日志不在这一步加,单独测出来的解码耗时不是有意义的指标,真正要盯的是子步骤 6.3/6.4 的"按键到画面"端到端链路。落地为 `Result<DecodedImage, DecodeError>`(`FileNotFound`/`DecodeFailed` 两种取值,沿用全局 `Result<T,E>` 约定,不用异常),不碰数据库,直接挂进 `core/api.h` 门面。配了一条明确标注"临时调试用"的一次性子命令 `pzt decode <jpeg_path>`(解码一张图打印尺寸和像素字节数就退出),供 6.2 的 Kitty 渲染器接线前先验证解码管线本身是通的;这条命令在 6.4 全键盘循环接上真正的渲染路径之后会退休。单元测试用 `CGImageDestination` 现场编码纯色 JPEG 作夹具,不在仓库里塞二进制图片资源,覆盖尺寸解码、纯色像素值在有损压缩下的合理误差范围、文件不存在、非图片内容三种情形;另外用 `screencapture` 生成的真实 2940×1912 JPEG 跑了一遍 `pzt decode` 做端到端验证
     - **6.2(已完成) Kitty 渲染器**(`cli`):新增 `cli/kitty`,复用探针验证过的 `t=t` 传输介质 + Tmux DCS passthrough 检测,包成 cli 侧可复用的渲染组件。核心接口:`detect_terminal_mode()` 在启动时探测一次运行环境(`$TMUX` 是否存在;存在则再用 `popen` 跑一次 `tmux show-options -gqv allow-passthrough` 查询 passthrough 是否开启),`render_rgba_via_tmpfile()` 拿探测结果和一张 `core::decode::DecodedImage` 发送到目标 fd——不在 Tmux 内时直接发送、不做包装也不查 passthrough;在 Tmux 内且 passthrough 未开启时直接返回 `RenderError::PassthroughDisabled`(不发送任何字节),由调用方给出"加 `set -g allow-passthrough on`"的明确提示,这个探测与分支逻辑按照设计只放在 `cli`,不下沉进 `core`。配了一条明确标注"临时调试用"的一次性子命令 `pzt render <jpeg_path>`(解码一张图渲染到终端就退出,状态信息打 stderr、stdout 只留给 Kitty 协议字节,和探针的约定一致),供 6.4 全键盘循环接线前先验证渲染管线本身是通的。单元测试(`cli/tests/kitty_test.cpp`)覆盖 `tmux_wrap` 的 DCS 包装与 ESC 转义、`base64_encode` 对照 RFC 4648 已知向量、`parse_allow_passthrough` 的 on/off/空/大小写异常输入、以及 `render_rgba_via_tmpfile` 在 passthrough 关闭时拒绝发送与在允许时正确写临时文件+控制序列这两种路径,不依赖真实 tmux 会话。另外用 `screencapture` 生成的真实 2940×1912 JPEG 跑了一遍 `pzt render` 做端到端验证,依次覆盖了文件不存在、非图片内容、Tmux 内 passthrough 关闭(本机默认状态,验证报错且不发送字节)、临时开启 `allow-passthrough` 后验证正常发送 DCS 包装后的控制序列、以及用 `env -u TMUX` 模拟独立 Ghostty 窗口模式验证不带包装直接发送这五种情形,验证后已把 `allow-passthrough` 恢复回本机原有的 `off` 设置
     - **6.3(已完成) 预取/缓存环形缓冲区**(`core/browse`,`jthread`):新增 `core/browse/prefetch.h`/`.cpp`,`PrefetchCache` 用一个 `std::jthread` 驱动的后台 worker 循环围绕当前浏览位置前后各 `window` 张调度 `core/decode` 解码,只负责调度("什么时候解码、结果放哪"),不重复实现解码本身。核心接口:`set_current(images, current_id)` 在每次导航后调用,按跟 `next_image`/`prev_image` 一致的循环折返语义重新算出窗口、异步调度窗口内缺失的解码任务、驱逐窗口外的缓存条目,不阻塞;`get(id)` 命中已解码条目直接返回,不在窗口内直接返回 `FetchError::NotInWindow`(不做同步解码,不能把解码算进按键触发的同步延迟预算),在窗口内但还没解码完时阻塞等待,这段等待就是预取没跟上时"按键到画面"链路的真实延迟。`DecodeFn` 做成可注入的构造参数(默认 `decode::decode_jpeg_file`),测试用假实现替换真实解码,不依赖真实 JPEG 文件与解码耗时。这是真正需要配延迟日志的延迟敏感路径:每次后台解码完成、每次 `get()` 命中/未命中都往 stderr 打一行耗时日志(`core/browse/prefetch.cpp` 的 `log_decode`/`log_get`)。单元测试(`core/tests/prefetch_test.cpp`)覆盖窗口内解码结果正确、窗口外返回 `NotInWindow`、解码失败传播为 `DecodeFailed`、`window` 超过图片总数时的环绕折返、`current_id` 为空/不在列表里时清空窗口、窗口移动导致条目被驱逐后重新进窗口会重新调度解码、以及 `get()` 在后台解码完成前真的会阻塞(用条件变量控制的假解码器验证)七种情形。为了在 `cli` 侧有一条真实可跑的路径验证这一步,把 `PrefetchCache`/`FetchError` 挂进 `core/api.h` 门面,配了一条明确标注"临时调试用"的一次性子命令 `pzt prefetch <project_name> [window]`(沿浏览顺序走一遍项目里的全部图片,每一步都调用 `set_current`+`get()`,打印每张图解码结果,increment 6.4 全键盘循环接上真正的渲染路径之后这条命令就可以退休);用 5 张 `sips` 生成的真实 JPEG(600x400)建了个项目跑了一遍 `pzt prefetch`,验证了预取窗口随导航移动、后台解码线程确实把像素准备好、以及延迟日志输出正常
     - **6.4 全键盘交互循环**(`cli`):把预取缓存 + Kitty 渲染器 + 已有的 core 浏览/标签/导出接口接到一起,`pzt open` 从桩实现变成真正的每日可用界面;`pzt tag`/`pzt browse` 这批临时调试子命令在这一步之后就该退休了
     - **6.5(视时间/需要而定的收尾项,不阻塞前四步跑通)**:降采样兜底阈值调优、`t=s` 共享内存传输介质验证——这两个都在"待确认问题"里放了很久,真机验证数据积累后再定
   - 单元测试贯穿每个 increment,不是最后单独一步:目前已覆盖 `Result<T,E>`、schema 幂等性、项目递归扫描、重名/空目录报错、按名字/按路径查找、`open`/`archive`/`delete` 的桩行为与级联删除、`rescan_project` 的新增/幂等、默认 `prune` 清除磁盘上消失的文件并级联清掉标签、`prune=false` 保留旧的只增不减行为、标签 CRUD/cap 校验/超限替换/幂等增删、浏览导航的两端循环折返与"未打标签"循环扫描不死循环、按标签过滤的排序规则(对应验收标准第 7 条,后续 increment 持续补充)
3. **`cli` 实现**:
   - 七个正式子命令(`new` / `rescan` / `open` / `list` / `export` / `archive` / `delete`),其中 `new`/`open` 的路径/项目名参数都是可选的:`new` 省略 `folder_path` 时取 cwd 为项目根路径,目录内无 JPEG 时报错;`open` 省略 `project_name` 时按 cwd 反查项目,带名字时按名字直接查找,两种情况找不到都报错并提示用 `list` 查看可用项目
   - 基于渲染延迟验证结论的 Kitty 渲染器(优先 `t=t`,超高像素走降采样兜底路径),启动时探测 `$TMUX` 决定是否需要 DCS passthrough 包装与 `allow-passthrough` 检测,同时支持 Tmux 窗格与独立 Ghostty 窗口两种运行模式
   - 全键盘浏览循环(`h`/`l`/`j`/`k`/`space`/`x`/`g`+数字/`q`)
   - 状态栏超限替换 UX,不使用会打断心流的弹窗
   - 默认硬编码快捷键——配置文件自定义按 M0_PRD.md 原话属于"后续"范畴,不进 M0
4. **集成与验收**:
   - 500+ 张图片的端到端延迟基准,与探针结论对照
   - 逐条核对 M0_PRD.md 的六条验收标准
   - 用真实素材(而非合成测试图)试用一轮

### tmux 焦点切换时清理图片残留(6.4 阶段真机测试发现并修复)

真机测试发现:在 tmux 里打开 `pzt open` 之后切到另一个 tmux 窗口,图片会一直挡在那个窗口里,不会消失。根因:Kitty 协议的图片 placement 是叠加在文字网格之上的独立合成层,tmux 本身不理解这一层——即便开了 passthrough 转发字节,tmux 切换窗口时只会重绘新窗口的文字内容,不会主动清掉上一个窗口留下的图片;而我们的进程这时候还在跑,只是阻塞在 `read()` 等键,不知道自己被切走了。之前"退出时清一次 placement"的修复只覆盖了正常按 `q` 退出这条路径,对这种情况没用。

第一版实现用的是 tmux 的 `pane-focus-out`/`pane-focus-in` hook,真机测试确认没用,而且找到了明确原因:这两个 hook 依赖真实终端级别的焦点事件——`focus-events` 选项打开后,tmux 向外层终端请求转发的是"这个 OS 窗口有没有失去焦点"这个真实终端事件,不是 tmux 自己内部"当前显示哪个 window"这个状态;在同一个 Ghostty 窗口里切 tmux 窗口(`next-window`/`previous-window`,或者任何自定义前缀键绑定的等效操作)根本不会产生真实的终端焦点事件,自然不会触发这两个 hook。进一步查证发现 `after-next-window`/`after-previous-window` 这类命令级 hook 在实测的 tmux 3.7 上也不是有效的 hook 名字(只有 `after-select-window` 被接受,但 `next-window`/`previous-window` 并不会触发它,说明这两个命令内部并不是走 `select-window` 实现的)。

最终改用轮询:`cli/term/tmux_focus.h` 的 `TmuxFocusWatcher` 起一个 `std::jthread` 后台线程,每隔约 300ms(拆成 50ms 一段的短睡眠,保证 `request_stop` 后能很快退出,不重蹈 increment 6.3 那次 worker 关闭延迟的覆辙)用 `tmux display-message -p -t <pane> '#{window_active}'` 查一次"我这个 pane 所在的 window 现在是不是正在显示给客户端"——`fork`+`execlp` 直接调用,不经过 `/bin/sh`,读子进程 stdout 判断是不是 `"1"`。状态从 1 变 0 时给自己发 `SIGUSR1`(失焦),从 0 变 1 时发 `SIGUSR2`(重新获焦)。这两个信号本来就是为了打断主线程阻塞中的 `read()`(信号处理函数只做异步信号安全的事——设置一个 `std::atomic<bool>` 标记;注册信号时特意不设 `SA_RESTART`,保证 `read()` 真的会被打断返回 `EINTR`),真正的清理/重绘逻辑放在主循环里:`read()` 因为信号被打断时,检查这两个标记——失焦清一次 placement,重新获焦时不等下一次真实按键就跳回外层循环补一次全量重绘。用 `TMUX_PANE` 环境变量(tmux 给 pane 内进程自动设置)确定"当前是哪个 pane",不需要另外查询。

用 `tmux pipe-pane`(捕获指定 pane 自己的原始输出,不受客户端当前在看哪个窗口影响)在一个隔离的真实 tmux 会话里验证过完整链路:切走后能看到多出来的一次 `a=d`(清除),切回来后能看到多出来的一次 `a=T`(补画),都不需要真实按键触发。

## 待确认问题

- 技术选型一节的三个选择(SQLite C API、手写参数解析、doctest)需要在开始脚手架搭建前明确签字,而不是隐式默认
- `t=s` 共享内存传输介质是否值得在预取/渲染模块实现阶段投入验证,目前只是探针结论里提出的一个方向,没有实测数据
- （部分已在 increment 6.4 落地)降采样:没有做成单独的"从多少 MP 开始触发"阈值路径,而是在 `pzt open` 每帧渲染时,直接用三面板布局已经算出来的 `fit_within` 目标像素尺寸,调用新增的 `core::decode::resize_rgba` 把图缩小到面板大致能显示的尺寸再传给终端(减少终端侧要读取/解码/合成的数据量)。真机测试确认这个改动生效(传输的 `s=`/`v=` 确实变小了),但切换图片依旧能感觉到明显卡顿,没有完全解决——本节最后一条新记的问题就是这个残留延迟
- 切图卡顿的残留问题(6.4 阶段真机测试发现,降采样后仍未解决):`key-to-render` 延迟日志测的是"读到键到我们的 `write()` 系统调用返回"这段时间,prefetch 命中时这个数字接近 0,说明我们自己这边(解码、缩放、拼控制序列、写 tmpfile)都不是瓶颈;`write()` 返回只代表数据交给了内核的 pty 缓冲区,不代表 Ghostty 已经真正读完临时文件、解码、合成显示完毕。而这条"终端真正显示完成"的时间,目前完全测不出来——为了修复 6.4.1 那次真实的死循环(终端协议响应被误当成按键消费),已经在控制序列里加了 `q=2`,终端不会再通过 stdin 回任何响应,这意味着我们没有任何信号能知道终端何时真正处理完一帧。下一步大概率要从"要不要有条件地打开协议响应、并且设计一套跟用户按键输入分开的分析通道"这个方向去调查,但目前只是记录问题,没有实测数据支撑具体做法,不在这里臆测方案。**重要更新**:排查 `q` 退出延迟问题(见下一条)时发现,本项目 Debug 构建默认带 `-fsanitize=address,undefined`,ASan 对每次内存访问都插桩、对大块内存(解码出来的 RGBA 缓冲区常有几 MB)的释放还有额外的 poison/quarantine 开销——真机测试这一路用的都是默认 Debug 构建,"切图卡顿"这个观感有多少是真实架构瓶颈、有多少是 ASan 本身的插桩开销,目前完全没有分离验证过。下次继续查这个问题之前,应该先用 `cmake -B build-release -DCMAKE_BUILD_TYPE=RelWithDebInfo` 建一个不带 sanitizer 的版本对照测一遍手感,再决定要不要继续往"降采样还不够"这个方向深挖
- `q` 退出后进程要卡 1-2 秒才真正退出(6.4 阶段真机测试发现,已修复大半):定位到两个原因。(1)`core::browse::PrefetchCache::worker_loop` 里 `cv_.wait(lock, stop, predicate)` 的谓词只看"队列是否非空",如果 `stop` 请求到达时队列里还排着好几张图(比如刚打开项目、还没等预取窗口跑完就按了 `q`),谓词早就是 true,不会因为 `stop` 被请求就提前返回——worker 会把队列剩下的每一张图都解码完才注意到 `stop`。已修复:在弹出新任务前显式加一道 `if (stop.stop_requested()) return;`,一旦 `stop` 被请求,不管队列还有多少排队项,都不再捡新的,最多等"已经弹出、正在解码"的这一张解码完。(2)修完 (1) 之后用 `sample` 抓了一次真实卡顿场景的线程栈,发现关掉 ASan(`-DCMAKE_BUILD_TYPE=RelWithDebInfo`)之后,同样的操作退出延迟从 2000+ ms 降到 72ms——运行中的 Debug+ASan 构建里,`~PrefetchCache()` 析构 `unordered_map` 时释放那几张已缓存的大图缓冲区,本身就要占大头,这基本是 ASan 的内存插桩/quarantine 开销,不是产品代码的真实瓶颈,不需要为这个专门优化生产路径
- （已在 increment 6.1 落地)色彩管理缺口如何处理:确认相信 CoreGraphics 默认的 ColorSync 色彩匹配行为(`CGContextDrawImage` 从源 `CGImage` 画到目标 device RGB context 时会自动做色彩匹配),不额外写 ICC 读取/转换代码,把这当作 M0 阶段"够用"的答案;以后如果真机使用中看到广色域素材出现可感知色偏,再回头评估要不要做显式 ICC 处理
- `pzt open` 支持按 cwd 反查项目(参数省略时)的设计,绕开了"子进程无法改变父 shell cwd"这个原本会需要 shell 集成方案才能解决的问题——`pzt new` 可以用 cwd 建项目、`pzt list` 展示根路径让用户自己 `cd` 过去,`open` 再按 cwd 反查或按名字直接查找,整条链路都不需要 `pzt` 反过来操纵 shell,已确认不需要额外的 fish function 包装。带名字的 `open <project_name>` 形式保留下来,是为了不强制用户必须先 `cd` 到项目目录才能打开——两种方式并存,互不排斥
- `archive` 目前没有对应的"取消归档"操作,如果实际使用中发现归档是个常见的误操作或者需要撤销的场景,需要补一个 `unarchive`,本文档暂不预先加这个接口
- 导出目标文件夹如果设在项目 `root_path` 内部(比如项目文件夹下建个 `exported/` 子目录),下次 `pzt rescan` 会把导出产物(复制体,或者 `--link` 模式下的符号链接)当成磁盘上新出现的照片扫进 `images` 表——`rescan` 是对 `root_path` 做全量递归扫描,导出文件的路径/文件名跟已知源文件不一样,会被当成合法新照片补录进来,污染项目、让这些没打过标签的衍生重复图出现在 `next_untagged` 里,`--link` 模式下还会出现两条 `images` 记录指向同一份物理文件。increment 5 没有处理这个问题,只在这里记录;目前的应对方式是提醒用户导出目标文件夹设在项目文件夹之外,以后要不要在代码层面处理(比如禁止 `output_folder` 是 `root_path` 的子路径、或者 rescan 时排除已知导出目录)留待后续再定
