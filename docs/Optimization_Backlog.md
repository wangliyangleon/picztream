# PZT 优化/重构待办

M1 收尾、M2（LibRaw 接入）开始前做的一次全面 review，记录发现的可优化点。每条标 size（S=小/低风险、M=中/需要规划、L=大/影响面广）和 status（本轮完成/待排期/观察-暂不处理）。这份文档只记录待办和已知取舍，不是设计文档——具体实现细节以动手那一轮的 commit/plan 为准。

## 本轮完成

### 提取 `live_versions_for_menu`（size: S）

**问题**：`handle_pick_version_to_apply_prompt`、`handle_pick_version_to_delete_prompt`、`handle_r_create_flow` 三处各自重复同一段代码——查 `list_versions(preset_id)` → 过滤掉已软删除的 → 截断到 9 个（交互菜单单个数字键的寻址上限）。

**处理**：提成一个 `live_versions_for_menu(preset_id)`，跟文件里已有的 `presets_for_menu()`/`tags_for_menu()` 命名风格一致。纯代码搬移，不改变行为。

### 提取 `prompt_and_read_key`（size: S）

**问题**：`handle_space_key`、`handle_g_export_flow`、`handle_g_key_prompt`、`handle_pick_preset_prompt`、`handle_pick_version_to_apply_prompt`、`handle_r_key` 六处都重复"拼好 `line` → `move_cursor` → `write_stdout(pad_to(line, content_cols))` → `read_one_byte()`"这个尾巴。

**处理**：提成一个 `char prompt_and_read_key(const std::string& line, int banner_row, int start_col, int content_cols)`。纯代码搬移，不改变行为。

### 图片在面板内居中 + padding（size: S）

**问题**：`cmd_open` 渲染块里，图片用 `fit_within` 算出保持长宽比的最大尺寸后，无条件画在图片区域的左上角——长宽比跟面板不完全匹配时（几乎总是这样），空白全部堆在右边/下边，图片贴着上边框和左边框。

**处理**：预留固定 padding（水平 2 列、垂直 1 行——终端 cell 不是正方形，横向数值上略大一点视觉才均衡），从 `image_cols`/`top_rows` 里减掉再传给 `fit_within`；用算出的 `target_cols`/`target_rows` 计算居中偏移，画图时 `move_cursor(image_top_row + offset_rows, start_col + 1 + offset_cols)`。不影响 `clear_placement`（按 image_id 清除,不依赖位置）、不影响信息栏排版、不需要改 `core`。

## 已完成

### 拆分 `cli/main.cpp`（size: L）— 已完成

`main.cpp` 从 1867 行拆成一个 33 行的纯分发器,其余按功能分到:`cli/text/`(纯文本工具,独立静态库 + `text_test.cpp` 单测)、`cli/ui/`(终端 io 原语,静态库,依赖 text)、`cli/menu/{tag_menu,filter_menu,recipe_menu}`(三套交互菜单,静态库 cli_menu)、`cli/commands/{commands,browse}`(小命令 + `pzt open` 主循环,编进可执行文件)。命名空间 `pzt::cli::{text,ui,menu,commands}`,依赖是无环 DAG(text ← ui ← menu ← commands/browse)。纯代码搬移,零行为变化;唯一增量是新增的 text 单元测试(9 个用例)。现在单文件最大是 `browse.cpp` 648 行(cmd_open 本身就是个大函数,不再进一步拆)。

## 待排期

### ~~拆分 `cli/main.cpp`~~（已完成,见上）

（原始分析保留在下,供参考。）`cli/main.cpp` 曾是 1867 行,全项目唯一超过 500 行的文件。内容上切成了几块:

- usage/帮助文本（`print_usage`/`print_tag_usage`/`print_recipe_usage`）
- UTF-8/显示宽度工具函数（`is_wide_codepoint`/`decode_utf8_at`/`display_width`/`pad_to`）
- tag 菜单交互（`handle_space_key` 及其子流程：`handle_cap_replace_submenu`/`handle_remove_tag_submenu`/`handle_create_tag_flow`/`handle_delete_tag_submenu`/`handle_add_tag_result`）
- `r` 菜单交互（`handle_r_key` 及其子流程：`handle_pick_preset_prompt`/`handle_pick_version_to_apply_prompt`/`handle_pick_version_to_delete_prompt`/`handle_r_create_flow`）
- `g` 菜单交互（`handle_g_key_prompt`/`handle_g_export_flow`）
- `cmd_open` 主循环本身（最大的一块，本身就有 500+ 行）
- 其余 `cmd_*` 子命令（`cmd_new`/`cmd_list`/`cmd_archive`/`cmd_delete`/`cmd_rescan`/`cmd_export`/`cmd_tag`/`cmd_recipe`）+ `main()`

拆分是纯代码搬移，不改变任何逻辑，但部分内部辅助函数（`read_one_byte`/`move_cursor`/`write_stdout`/`pad_to`/`expand_home_path`/`read_text_line` 等）需要提升到一个共享的内部头文件，`cli/CMakeLists.txt` 要加新源文件。工作量比上面三条大一截，需要单独一轮规划"每个函数该归到哪个文件"，不建议临时穿插着做。

### 多语言支持：默认中文可切换英文（size: L）

**现状**：`cli/main.cpp` 里散落着几十处中文字符串字面量——固定帮助文本、banner 提示（`kBannerText`）、十几个 `handle_*` 交互函数里拼出来的菜单行和状态提示（大多是"固定中文片段 + 拼接动态内容"）。`core/` 完全不含任何面向用户的文本——所有报错都是结构化 enum，转成人话是 `cli` 的职责。i18n 的边界天然跟已有的 core/cli 分层重合，不需要碰 `core/`。

**设计方向**：不引入 gettext/ICU 这类重量级框架——两种语言、字符串数量可控（~100 处），用一个全局 `enum class Lang { zh, en }` + 一个 `cli/i18n/` 模块，每条文本对应一个小函数（不是扁平的 key-value 表 + printf 风格模板）：有动态内容的文本直接写成接受那些参数的函数，编译期就能检查参数对不对,比运行时字符串模板解析更符合项目"不要类型不安全的灵活性"这个一贯取舍(对照 `core/recipe/recipe.h` 里明确拒绝用 JSON blob 存调整参数的理由)。语言选择在启动时读一次(`--lang` 参数或环境变量),存成全局变量,运行期不切换(离线全键盘工具,没有"运行中途切换语言"的真实场景)。按现有 `handle_*` 函数分组织文件,不追求一个大一统的字符串表。

**规模判断**：机械但大面积——要触达 main.cpp 里几乎每一个 `handle_*` 函数和所有 `cmd_*` 的错误分支。建议排在"拆分 `cli/main.cpp`"之后做：模块边界先理清楚，之后把文本迁移到 `cli/i18n/` 时改起来更顺。

## 观察到但不建议现在动的点

### `core::api` 每次调用都新开一个 sqlite3 连接

包括 `pzt open` 每一帧重画时 `tags_for_image`/`get_image`/`get_image_recipe`/`describe_recipe` 都会各自开关一次连接。这是从 M0 就有的一贯模式，不是这次新发现的问题；M1 验收时实测过 `RelWithDebInfo` 下 `key-to-render` 稳定在 60ms 左右、主观无感知延迟，没有测出真实性能问题，纯粹是"看起来有点浪费"的架构观察。不建议现在改——按项目"不做过早优化"的原则，等真的有实测数据支撑（比如项目规模变大、换成网络存储的家目录）再考虑引入一个贯穿单次 CLI 调用的共享连接。

### 交互菜单函数缺少自动化测试

`r`/`g`/`space` 这些 `handle_*` 函数目前只能靠真机手动验证。这在 `docs/M1_Eng_Design.md` increment 6 里已经明确记录为已知局限，不是这次新发现。

### M2 前瞻：`core::color`/`core::decode` 的解耦现状

当前的 `DecodedImage`（8-bit RGBA）接口跟具体的 JPEG 解码路径已经解耦得不错——`core::color` 只认 `DecodedImage`，不关心它是怎么来的。M2 引入 LibRaw 产出线性 `uint16_t` RGB 矩阵时，大概率是加一条新的解码路径（可能是新的数据结构，因为位深不同），复用现有的 `color`/`recipe::render` 还是需要 M2 自己的设计决定。现状已经足够解耦，不是需要现在处理的债务，无需预先改动。
