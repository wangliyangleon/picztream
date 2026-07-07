# PZT 优化/重构待办

M1 收尾、M2（LibRaw 接入）开始前做的一次全面 review，记录发现的可优化点。每条标 size（S=小/低风险、M=中/需要规划、L=大/影响面广）和 status（已完成/待排期/观察-暂不处理）。这份文档只记录待办和已知取舍，不是设计文档——具体实现细节以动手那一轮的 commit/plan 为准。

## 已完成

### 提取 `live_versions_for_menu`（size: S）

**问题**：`handle_pick_version_to_apply_prompt`、`handle_pick_version_to_delete_prompt`、`handle_r_create_flow` 三处各自重复同一段代码——查 `list_versions(preset_id)` → 过滤掉已软删除的 → 截断到 9 个（交互菜单单个数字键的寻址上限）。

**处理**：提成一个 `live_versions_for_menu(preset_id)`，跟文件里已有的 `presets_for_menu()`/`tags_for_menu()` 命名风格一致。纯代码搬移，不改变行为。

### 提取 `prompt_and_read_key`（size: S）

**问题**：`handle_space_key`、`handle_g_export_flow`、`handle_g_key_prompt`、`handle_pick_preset_prompt`、`handle_pick_version_to_apply_prompt`、`handle_r_key` 六处都重复"拼好 `line` → `move_cursor` → `write_stdout(pad_to(line, content_cols))` → `read_one_byte()`"这个尾巴。

**处理**：提成一个 `char prompt_and_read_key(const std::string& line, int banner_row, int start_col, int content_cols)`。纯代码搬移，不改变行为。

### 图片在面板内居中 + padding（size: S）

**问题**：`cmd_open` 渲染块里，图片用 `fit_within` 算出保持长宽比的最大尺寸后，无条件画在图片区域的左上角——长宽比跟面板不完全匹配时（几乎总是这样），空白全部堆在右边/下边，图片贴着上边框和左边框。

**处理**：预留固定 padding（水平 2 列、垂直 1 行——终端 cell 不是正方形，横向数值上略大一点视觉才均衡），从 `image_cols`/`top_rows` 里减掉再传给 `fit_within`；用算出的 `target_cols`/`target_rows` 计算居中偏移，画图时 `move_cursor(image_top_row + offset_rows, start_col + 1 + offset_cols)`。不影响 `clear_placement`（按 image_id 清除,不依赖位置）、不影响信息栏排版、不需要改 `core`。

### 拆分 `cli/main.cpp`（size: L）

`main.cpp` 从 1867 行拆成一个 33 行的纯分发器,其余按功能分到:`cli/text/`(纯文本工具,独立静态库 + `text_test.cpp` 单测)、`cli/ui/`(终端 io 原语,静态库,依赖 text)、`cli/menu/{tag_menu,filter_menu,recipe_menu}`(三套交互菜单,静态库 cli_menu)、`cli/commands/{commands,browse}`(小命令 + `pzt open` 主循环,编进可执行文件)。命名空间 `pzt::cli::{text,ui,menu,commands}`,依赖是无环 DAG(text ← ui ← menu ← commands/browse)。纯代码搬移,零行为变化;唯一增量是新增的 text 单元测试(9 个用例)。现在单文件最大是 `browse.cpp` 648 行(cmd_open 本身就是个大函数,不再进一步拆)。

### 多语言支持：默认中文可切换英文（size: L）

新增 `cli/i18n/`（`pzt::cli::i18n`）模块，函数式设计（121 个函数，不是模板字符串），`enum class Lang { zh, en }` + 全局 `g_lang`，`init_lang()` 在 `main()` 里调一次：优先读 `PZT_LANG` 环境变量，没设的话退化到系统 `LANG` 里找 `zh`/`en` 关键字，都没有默认 `zh`。`core/` 完全没碰，边界跟 core/cli 分层重合。

**验收时发现并修复的问题**（供以后类似任务参考）：
- 4 个"缺少 <project_name>"错误提示当初直接照抄了拆分前就遗留的英文字面量，没加中文分支，中文模式下会冒出英文——已补上 `g_lang` 判断。
- "废片"系统标签是真正持久化在数据库里的 `core::tagging::kRejectTagName` 字面量（`core/tagging/tagging.h`），不是纯 cli 生成的文本——`tags_for_image`/`list_tags` 读出来的 `TagSummary::name` 永远是那个中文字面量，不会跟着语言设置变。原来的实现只在"0:废片"这个固定菜单槽位的三处内联判断里处理了显示文案，但 `pzt tag list` 和浏览界面信息栏的"标签:"列表都是直接打印 `t.name`，英文模式下这两处依然会漏出裸的"废片"。修法是新增 `i18n::tag_display_name(tag)`（`is_system` 为真时换成 `i18n::reject_tag_label()`，否则原样返回），在这两个直接显示 `t.name` 的地方套用；同时把三处内联的 `g_lang == zh ? "废片" : "reject"` 三元表达式收敛成 `reject_tag_label()` 一个函数，不再散落重复。
- 两处注释在编辑过程中被误改出"的"→"of"的中英混杂病句，已改回。
- `i18n_test.cpp` 有个用例直接改全局 `g_lang` 又没在结束前还原，补了还原。

**教训**：像"废片"这种同时是*真实持久化数据*又要在 UI 上展示的字符串，不能只在生成菜单文案的地方做 i18n，任何直接透传 `core` 返回的原始字段做展示的地方都要单独检查是不是需要做一次"系统标签 → 翻译后的显示名"的替换。

**已知残留、这次没动**：`msg_tag_item` 里的 `ordered`/`system`/`cap=` 这几个技术性后缀标签在中英文模式下都是英文——这是拆分前就有的既有写法（不是这次 i18n 引入的新问题），值不值得也翻译成中文是个待定的风格问题，不在这次修复范围内。

## 观察到但不建议现在动的点

### `core::api` 每次调用都新开一个 sqlite3 连接

包括 `pzt open` 每一帧重画时 `tags_for_image`/`get_image`/`get_image_recipe`/`describe_recipe` 都会各自开关一次连接。这是从 M0 就有的一贯模式，不是这次新发现的问题；M1 验收时实测过 `RelWithDebInfo` 下 `key-to-render` 稳定在 60ms 左右、主观无感知延迟，没有测出真实性能问题，纯粹是"看起来有点浪费"的架构观察。不建议现在改——按项目"不做过早优化"的原则，等真的有实测数据支撑（比如项目规模变大、换成网络存储的家目录）再考虑引入一个贯穿单次 CLI 调用的共享连接。

### 交互菜单函数缺少自动化测试

`r`/`g`/`space` 这些 `handle_*` 函数目前只能靠真机手动验证。这在 `docs/M1_Eng_Design.md` increment 6 里已经明确记录为已知局限，不是这次新发现。

### `core/export/export.cpp` 的跳过原因是硬编码中文

`ExportSkipped::reason` 的四个取值（"源文件缺失"/"解码失败"/"应用风格失败"/"编码失败"）是 `core` 层硬编码的中文字面量，完全在 `cli/i18n` 够不着的地方——英文模式下导出跳过提示会中英文夹杂。这是 M1 时代就有的设计（`core` 层直接塞了展示用字符串，没有走结构化 enum），这次 i18n 任务正确地没有碰 `core/`，但这个洞会一直漏。要彻底解决得把 `ExportSkipped::reason` 改成枚举、翻译挪到 cli 层——量级不小，值得单独判断要不要做，不在这次范围内。

### M2 前瞻：`core::color`/`core::decode` 的解耦现状

当前的 `DecodedImage`（8-bit RGBA）接口跟具体的 JPEG 解码路径已经解耦得不错——`core::color` 只认 `DecodedImage`，不关心它是怎么来的。M2 引入 LibRaw 产出线性 `uint16_t` RGB 矩阵时，大概率是加一条新的解码路径（可能是新的数据结构，因为位深不同），复用现有的 `color`/`recipe::render` 还是需要 M2 自己的设计决定。现状已经足够解耦，不是需要现在处理的债务，无需预先改动。
