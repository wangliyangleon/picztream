# PicZTream (PZT) Milestone 1 工程设计文档

## 背景

`docs/M1_PRD.md` 已经拍板了 M1 的产品决策（两层 recipe 模型：内置预设 + 用户在预设基础上调整细节保存的 version，软删除，`r` 前缀键，命令行只留 `list`/`rename`/`delete`），但按 `AGENTS.md` 的工程契约，具体的表结构、模块划分、接口签名要落到这份文档，PRD 本身明确把这些留给 Eng Design。implementation 应在本文档评审通过后再开始。

## 色彩流水线性能验证结论

对应 M1_PRD.md"技术方案概要"提到的 Phase 0 spike，已经用一个独立探针（`spikes/color_lut_probe/`）验证过，完整数据见该目录下的 `results.md`，这里摘要对设计有直接影响的结论：

1. **预览分辨率下色彩处理完全不需要异步或多线程**：两档预览尺寸（1.09MP/2.46MP），最贵的 3D LUT 三线性插值单线程也只要 10-22ms，比 100ms 目标有 5-10 倍余量，白平衡类简单调整单线程连 3ms 都不到。
2. **全分辨率处理需要多线程**：12MP 档单线程 LUT 应用已经踩线 100ms，60MP 档要 725ms；`jthread` 按行切分多线程能压到约 165ms（8 线程）。
3. **不引入 NEON SIMD**：没有实测数据支撑现在投入，`jthread` 多线程已经够用，符合"不做过早优化"的工程契约。
4. **3D LUT 网格大小（17³/33³）对性能几乎没有影响**：网格选多大是纯粹的调色精度权衡，不是性能权衡。
5. 探针只测了"LUT + 白平衡"两种运算；M1_PRD.md 定的 recipe 参数还包括高光/暗光调整，这两个和白平衡是同一种计算形状（逐像素 O(1)、无查表 gather），按类比可以信任同等便宜，不需要为此单独再跑一次 spike——但增量 4 落地时仍然要用真实的、串联起来的完整管线（LUT + 全部调整参数）跑一次 `key-to-render` 延迟验证，不能只依赖探针里单独测量的两个原语数字。

### 对 core 设计的直接影响

- 预览路径：色彩处理是同步操作，不需要后台线程、不需要缓存，直接挂在 `pzt open` 现有的每帧渲染路径里（`resize_rgba` 降采样之后、发给终端之前）
- 全分辨率路径（导出烘焙）：需要 `jthread` 并行，但不需要一个"比命令执行时间更长"的常驻后台队列——见"模块划分与并发模型"一节的说明
- 色彩处理的核心运算分两类：预设自带的 3D LUT（贵，per-pixel 8 次查表+插值）+ 用户可调的高光/暗光/白平衡参数（便宜，per-pixel O(1)），这个二元划分直接对应两层 recipe 模型的"预设"与"version 的调整量"

## 数据库 Schema 设计

一张新表 + 一次对既有 `images` 表的迁移。这是本项目第一次需要在已经发布过的表上加列——M0 的 `initialize_schema` 全是幂等的 `CREATE TABLE IF NOT EXISTS`，从来没处理过"给已存在的表加新列"这种情况，需要补一个轻量迁移机制（见下文"Schema 迁移机制"）。

```
recipes(
  id             INTEGER PRIMARY KEY,
  parent_id      INTEGER REFERENCES recipes(id) ON DELETE CASCADE,
                 -- NULL = 这一行是"预设"(第一层)；非空 = 某预设下用户保存的 version(第二层)
  name           TEXT,             -- 预设必须有名字(见下面的局部唯一索引)；version 的名字可选，允许 NULL/重复
  is_system      INTEGER NOT NULL DEFAULT 0,  -- 预设恒为 1，version 恒为 0
  base_lut_size  INTEGER,          -- 仅预设使用：3D LUT 网格边长 n
  base_lut       BLOB,             -- 仅预设使用：n*n*n*3 个 float32，行优先序列化
  highlights     REAL NOT NULL DEFAULT 0,     -- 仅 version 使用，预设恒为 0(预设自己就是中性状态)
  shadows        REAL NOT NULL DEFAULT 0,
  wb_shift_r     REAL NOT NULL DEFAULT 0,
  wb_shift_b     REAL NOT NULL DEFAULT 0,
  created_at     INTEGER NOT NULL,
  deleted_at     INTEGER           -- 仅 version 使用(软删除)，预设恒为 NULL(不可删除)
)
CREATE UNIQUE INDEX idx_recipes_preset_name ON recipes(name) WHERE parent_id IS NULL;
```

`images` 表迁移（新增一列，其余不变）：

```
ALTER TABLE images ADD COLUMN recipe_id INTEGER REFERENCES recipes(id) ON DELETE SET NULL;
-- NULL = Origin(不应用任何风格)，这是 M1_PRD.md 里"固定 0 号位"的落地方式：
-- 不是一张真实存在的 recipe 数据行，就是这一列取 NULL 的状态
```

补充说明：

- 用单张自引用表（`parent_id` 指向自己）而不是"预设表 + version 表"两张表，直接类比 M0 `tags` 表用 `is_system` 一列区分内置/用户标签的做法——两层结构本质上是同一种"名字 + 一组参数"的行，只是 `parent_id` 是否为空决定这一行该按预设的语义读（`base_lut`/`base_lut_size` 有意义）还是按 version 的语义读（`highlights`/`shadows`/`wb_shift_*` 有意义），不需要为此拆两张表、多一次 JOIN
- 预设的名字唯一性用局部唯一索引（`WHERE parent_id IS NULL`）而不是在 `name` 列整体加 `UNIQUE`——因为 version 的名字允许重复（甚至允许都不设名字，纯粹靠编号区分），一个覆盖全表的 `UNIQUE` 会错误地阻止两个不同预设下出现同名 version
- `recipes.id` 的升序就是创建顺序，`r` 菜单编号（预设 1-9、某预设下 version 的 1-9）都是查询时按 `id ASC` 现算出来的位置，不单独维护一个"编号"列——直接照抄 `tags_for_menu` 现有的做法（标签编号也是查询时按 `id` 排出来的，不是存储列），删除一个 version 之后后面的编号会跟着往前移，这跟现在删标签的行为一致，不是新引入的不一致
- `deleted_at` 只影响"能不能被选中应用/是否出现在 `r` 菜单的可选列表里"，不影响任何已经引用这个 version 的图片的渲染——这是 M1_PRD.md 定的软删除语义，`pzt recipe list` 仍然会显示已删除的 version（标注状态），直接复用 M0 `pzt list` 展示归档项目的既有模式
- `images.recipe_id` 用 `ON DELETE SET NULL` 而不是 `CASCADE`——理论上不会有图片引用一个真的被物理删除的 recipe 行（软删除的 version 只要还有图片引用就不会被真正 `DELETE`，这是"清理孤儿 version"这个可选功能自己的前提），但防御性地选一个"删了就变回 Origin"而不是级联删图片记录本身，更安全

### Schema 迁移机制（本项目第一次需要）

`core/db/schema.cpp` 新增一个小工具函数：

```cpp
bool column_exists(sqlite3* conn, const char* table, const char* column);
// 用 `PRAGMA table_info(<table>);` 查询，逐行比对列名

void ensure_column(sqlite3* conn, const char* table, const char* column, const char* ddl_fragment);
// column_exists 为 false 时执行 `ALTER TABLE <table> ADD COLUMN <ddl_fragment>;`
```

`initialize_schema` 在建完 `recipes` 表之后调用 `ensure_column(conn, "images", "recipe_id", "recipe_id INTEGER REFERENCES recipes(id) ON DELETE SET NULL")`。因为整个函数每次打开数据库都会跑一遍（`Database::open_at` 里的既有调用点），这个迁移本身也要做成幂等的——`column_exists` 检查就是幂等性的保证，新库（新增列已经在建表语句里，不会走到这条 `ALTER TABLE`）和老库（M0 时代建的、缺这一列的库）都能正确处理，不需要区分"新装用户"和"从 M0 升级的用户"两条路径。

内置预设的种子数据不放进 `schema.cpp`（那里只管表结构，不管业务内容），而是 `core/recipe/recipe.cpp` 提供 `ensure_default_presets(sqlite3*)`，在 `Database::open_at` 里紧跟 `initialize_schema(db)` 之后调用，用 `INSERT OR IGNORE`（基于预设名字的唯一索引）保证幂等，不会每次开库都重复插入。

**预设的 `base_lut` 怎么产生**：不需要一个独立的离线生成工具或者外部 LUT 文件——预设数量少（个位数），每个预设对应一段直接写在 `core/recipe/`（比如 `presets_seed.cpp`）里的小函数（`make_classic_chrome_lut()`、`make_proneg_lut()` 这种命名），函数体用色调曲线/颜色矩阵的公式算出 33³ 网格每个格点的输出颜色，跟 `spikes/color_lut_probe/probe.cpp` 里 `make_lut()` 用公式生成合成 LUT 是同一种做法，只是这次要写出真正体现每个预设调色意图的公式，不是随便一个用来测性能的扭曲。`ensure_default_presets()` 对每个预设调一次对应函数、把算出来的数组序列化写进 `base_lut`，只在数据库第一次初始化时执行一次（后续都被 `INSERT OR IGNORE` 挡住），不需要额外的构建步骤、外部资源文件或运行时计算开销。

## core/api 接口设计

### `core/recipe/`（新模块，presets/version 的 CRUD 与图片关联，纯业务逻辑，不碰像素）

```cpp
namespace pzt::core::recipe {

using RecipeId = std::int64_t;
using project::ImageId;

struct PresetSummary { RecipeId id; std::string name; };

struct VersionSummary {
  RecipeId id;
  RecipeId preset_id;
  std::optional<std::string> name;
  double highlights, shadows, wb_shift_r, wb_shift_b;
  bool deleted;
};

std::vector<PresetSummary> list_presets();               // 全局，不分项目——预设是工具级的
std::vector<VersionSummary> list_versions(RecipeId preset_id);  // 含软删除的，cli 自己按场景过滤

struct VersionParams { double highlights = 0, shadows = 0, wb_shift_r = 0, wb_shift_b = 0; };

enum class CreateVersionError { PresetNotFound };
Result<RecipeId, CreateVersionError> create_version(RecipeId preset_id,
                                                     std::optional<std::string> name,
                                                     VersionParams params);

enum class RecipeOpError { NotFound, IsPreset };  // 预设不可 rename/delete
Result<void, RecipeOpError> rename_version(RecipeId version_id, const std::string& new_name);
Result<void, RecipeOpError> delete_version(RecipeId version_id);  // 软删除，设置 deleted_at

void set_image_recipe(ImageId image_id, std::optional<RecipeId> recipe_id);  // nullopt = Origin/清除
std::optional<RecipeId> get_image_recipe(ImageId image_id);

// 把一个 recipe_id(可能是预设本身，也可能是某个 version)展开成"用哪个预设
// 的 LUT + 最终生效的调整参数"——version 的参数就是它自己这一行存的值，预
// 设本身(parent_id IS NULL 的那一行)展开出来的调整参数全部是 0。
struct ResolvedRecipe {
  int lut_size;
  const std::vector<float>* lut_data;  // 指向预设那一行的 base_lut，调用方不持有所有权
  VersionParams params;
};
std::optional<ResolvedRecipe> resolve_recipe(RecipeId recipe_id);  // 找不到时返回空，调用方按 Origin 处理

// 给定一张已解码的原图渲染出应用了某个 recipe 之后的效果，组合
// resolve_recipe + core/color 的像素运算，是 core/recipe 对外唯一需要碰
// core/color 的地方。thread_count=1 用于预览(同步)，导出烘焙传
// hardware_concurrency()。
enum class RenderRecipeError { RecipeNotFound };
Result<decode::DecodedImage, RenderRecipeError> render(const decode::DecodedImage& src,
                                                        RecipeId recipe_id,
                                                        unsigned thread_count = 1);

}  // namespace pzt::core::recipe
```

`resolve_recipe`/`render` 对预设自身和 version 一视同仁：如果 `recipe_id` 指向的行 `parent_id IS NULL`（就是预设本身），直接用它自己的 `base_lut` + 全零参数；如果指向一个 version，用 `parent_id` 找到所属预设拿 `base_lut`，调整参数用 version 自己这一行的值——两种情况在 SQL 层面是同一条"如果 `parent_id IS NULL` 就用自己，否则 JOIN 一次拿 parent"的查询，不需要两套代码路径。

### `core/color/`（新模块，纯像素运算，不碰数据库，不知道 recipe 是什么）

```cpp
namespace pzt::core::color {

struct Lut3D { int size; std::vector<float> data; };  // n*n*n*3 float，跟 recipe::ResolvedRecipe 对应

// 原地处理 decode::DecodedImage。thread_count=1 时单线程同步(预览路径)，
// >1 时按行切分到多个 jthread(导出全分辨率烘焙路径)，两条路径共用同一份
// 实现，不是两套代码。三线性插值算法、LUT 数值构造方式跟
// spikes/color_lut_probe/probe.cpp 里已经验证过的完全一致，这次是把它从
// spike 提升为生产代码。
void apply_lut(decode::DecodedImage& img, const Lut3D& lut, unsigned thread_count = 1);

// 高光/暗光/白平衡偏移，逐像素 O(1)，同样支持 thread_count 切分，但预览
// 分辨率下即使单线程也可忽略不计(Phase 0 spike 的白平衡数字可以直接类比)。
void apply_adjustments(decode::DecodedImage& img, double highlights, double shadows,
                        double wb_shift_r, double wb_shift_b, unsigned thread_count = 1);

}  // namespace pzt::core::color
```

### `core/decode/` 增补：JPEG 编码（导出烘焙需要，M0 阶段只有解码）

M0 从来没有"把像素写回 JPEG 文件"这个需求——导出功能只是复制/软链原始文件字节。M1 导出应用了 recipe 的图片时需要把处理后的像素编码回 JPEG，这个能力目前只在 `core/tests/decode_test.cpp` 的测试夹具里出现过（用 `CGImageDestination` 现场编码纯色 JPEG 作测试输入），这次把它提升成正式的生产函数：

```cpp
enum class EncodeError { EncodeFailed };
Result<void, EncodeError> encode_jpeg_file(const decode::DecodedImage& img, const std::string& path,
                                            double quality = 0.9);
```

沿用 `CGImageDestinationCreateWithURL` + `kCGImageDestinationLossyCompressionQuality`，跟解码用的是同一套 ImageIO/CoreGraphics 工具链，不引入新依赖。

### `core/export/` 修改

`export_tag` 遇到 `get_recipe(image_id)` 非空的图片时：解码原图（全分辨率）→ `recipe::render(..., thread_count = hardware_concurrency())` → `encode_jpeg_file` 写到目标路径，取代原来的 `fs::copy_file`/`fs::create_symlink`；没有应用 recipe 的图片继续走原来的复制/软链路径，字节级不变。`--link` 模式下，应用了 recipe 的图片没有"原始字节"可以软链，只能落地成一份真实文件——这是 M1 对既有 `--link` 语义的一个自然限制，写清楚在函数说明里，不需要额外的参数或分支。

## 模块划分与并发模型

`core/` 新增两个子模块：

- `core/recipe/`：presets/version 的 CRUD、软删除、图片关联、"resolve 一个 recipe_id 该用哪个 LUT + 参数"，业务逻辑，不碰像素
- `core/color/`：3D LUT 三线性插值 + 高光/暗光/白平衡的纯像素运算，不碰数据库，风格上跟 `core/decode/` 对齐（"字节/像素进，像素出"，同步调用，调用方决定要不要丢进线程）

**导出烘焙不需要一个常驻的后台队列**：Roadmap.md"全分辨率处理走异步队列"这句话，落地到 `pzt export` 这个具体场景时，实际含义是"给这一批图片里需要烘焙的部分分摊到多个 `jthread` 上跑，不要串行阻塞"，而不是一个"生命周期比命令执行时间更长、需要在多次命令调用之间保持运行"的后台服务——`pzt export` 本身就是一次性的批处理命令，执行完就退出，不存在"导出完之后还有未完成的后台工作"这种状态需要维护。`core::color` 的 `thread_count` 参数直接支持这一点：导出时对每张需要烘焙的图片传 `hardware_concurrency()`，让单张图片内部的 LUT 应用按行并行；如果未来发现"多张图片之间也应该并行处理"比"单张图片内部按行并行"更划算，可以在 `export_tag` 内部再加一层任务分发，但这次不预先做这个决定——现在的设计已经能让全分辨率处理不拖慢导出速度太多，没有实测数据支撑更复杂的调度。

预览路径完全不需要新的并发原语——直接在 `pzt open` 现有的每帧渲染逻辑里，`resize_rgba` 降采样之后、发给终端之前，调用 `recipe::render(..., thread_count = 1)`，同步执行。这一步不复用/不修改 `core/browse::PrefetchCache`（那个只负责"JPEG 字节 → 解码像素"这一步，职责保持不变），recipe 渲染是 prefetch 拿到解码结果之后、cli 渲染之前新加的一步，两者是流水线上前后相邻但互不干扰的两个阶段。

## 技术选型

沿用 M0 已确认的选型（SQLite C API、手写参数解析、doctest），没有新增依赖。JPEG 编码复用 ImageIO/CoreGraphics（`core/decode` 已经在用的同一套框架），不引入 libjpeg-turbo 等第三方库。

## 任务分解（Task Breakdown）

延续 M0 的节奏：每个 increment 结束都要能实际跑一条 `pzt` 命令手动验证，不是最后一起验收。跟 M0 一样，验证某个 core 能力但还没轮到对应的正式交互入口时，会先配一条明确标注"临时调试用"的命令，等真正的交互入口（`r` 前缀键）落地后统一退休，不提前暴露成正式接口。

1. **数据层：`recipes` 表 + `images` 迁移 + 内置预设种子**：落地"数据库 Schema 设计"一节的表结构、`ensure_column` 迁移机制、`ensure_default_presets`（先有 1-2 个预设即可，一个用恒等 LUT 验证管线通不通，另一个随便调一个偏色当第二个样本——预设内容本身的调色设计不在这次范围内，是后续可以随时补充、不阻塞其它 increment 的工作）。`core/recipe/recipe.h/.cpp` 只实现 `list_presets`/`list_versions`（此时 version 还没有创建入口，永远是空列表）。落地 `pzt recipe list`（先只显示预设那一层），验证种子数据正确写入、`ensure_column` 在已有 M0 时代旧库上跑不报错
2. **version 的增删改 + 软删除**：`create_version`/`rename_version`/`delete_version`，`pzt recipe list` 补全 version 那一层的展示（含"已删除"标注），落地正式命令 `pzt recipe rename <preset>:<version_number> <new_name>` 和 `pzt recipe delete <preset>:<version_number>`（寻址语法在这一步定下来，取代 PRD 里的待确认状态：`<version_number>` 是"该预设下按 id 升序、排除已软删除的排位"，跟 `r` 菜单里看到的编号保持一致）。配一条临时调试命令 `pzt recipe create-debug <preset> [--highlights N] [--shadows N] [--wb-r N] [--wb-b N] [--name NAME]`，因为正式的创建入口 `r c` 要到 increment 6 才有，这一步需要一个非交互的方式先把 `create_version` 测起来
3. **图片 ↔ recipe 关联**：`set_image_recipe`/`get_image_recipe`。配临时调试命令 `pzt recipe apply-debug <project_name> <image_relative_path> <preset>:<version_number>` 和 `pzt recipe clear-debug <project_name> <image_relative_path>`，验证多次独立进程重开后关联持久保留
4. **`core/color` 渲染管线 + `core/decode` 补 JPEG 编码**：`apply_lut`/`apply_adjustments`（`thread_count` 参数，单线程/多线程共用一份实现）、`encode_jpeg_file`、`core::recipe::resolve_recipe`/`render` 把两者接起来。单元测试覆盖 LUT 三线性插值的已知输入输出、高光/暗光/白平衡的边界值裁剪、`thread_count>1` 时结果跟单线程一致（并行只影响速度不影响正确性）。配一条临时调试命令 `pzt color-debug <jpeg_path> <preset>:<version_number> <output.jpg>`，解码→渲染→编码写出一个可以直接用 Preview.app 肉眼查看的文件，用真实照片跑一遍确认颜色变化符合预期，同时记一下这条完整链路（不是探针里孤立的两个原语）的总耗时,跟 Phase 0 spike 的数字做个量级对照
5. **预览接入 `pzt open` 现有渲染路径 + `r v` 查看切换**：在 `resize_rgba` 降采样之后、发给终端之前插入 `recipe::render(..., thread_count=1)` 这一步。顺带落地 `r` + `v`（临时切换当前图片是否应用风格，纯查看层面，不改 `recipe_id`，导航到其它图片后重置为默认展示风格化效果）——这是这一步唯一需要的 `r` 前缀键行为，不需要等 increment 6 的完整菜单：终端 cbreak 模式没有按键释放事件，"按住看原图"做不出来，退而求其次做成两次按键的切换键，见 `docs/M1_PRD.md`"应用与预览"一节的说明。信息栏"风格:"那一行的预设/version 名字在当前渲染的是风格化效果时加粗（ANSI `\x1b[1m`，包在 `pad_to` 算完宽度之后的结果外层，不能反过来，否则转义字节会被 `display_width` 当成可见字符算错宽度），切到原图预览时取消加粗。还没有完整的 `r` 菜单（应用/创建/删除留给 increment 6），先用 increment 3 的调试命令给某张图片手动设置一个 recipe，再 `pzt open` 该项目肉眼确认预览正确应用了风格、切换正确、加粗状态正确、`key-to-render` 延迟汇总仍在正常范围（对照 Phase 0 spike 的预期量级）
6. **`r` 前缀键完整交互**（已完成，对应 M1_PRD.md"应用与预览"一节）：统一的 `handle_r_key` 入口，一次性渲染出完整菜单行（`r:清除 v:预览原图 c:新建 d:删除` + 动态的预设 1-9 列表）再读一个字节分发，风格上对齐 `handle_space_key`。
   - **6.1 应用/清除（已完成）**：`r` + 预设数字进入第二层（`handle_pick_version_to_apply_prompt`，`0`=预设自身中性状态、`1-9`=该预设下未软删除的 version），`r` + `0`/`r` + `r` 走独立的快捷清除路径（直接 `set_image_recipe(image_id, std::nullopt)`，不经过预设列表）。应用/清除成功后静默（不返回状态文案），信息栏"风格:"那一行下一帧自然刷新，对齐 `handle_add_tag_result` 静默成功的既有做法
   - **6.2 创建（已完成）**：`r` + `c`——`handle_pick_preset_prompt`（三处需要"选预设"的地方共用的新函数，供应用/创建/删除三个流程复用）选基础预设，依次 `read_text_line` 读高光/暗光/白平衡红/蓝四个数值（留空或解析失败都当 0，不重试，风格对齐标签 cap 的宽松解析）+ 可选名称，调 `create_version`。创建之后**不会**自动应用到当前图片，对齐 `space c` 建标签不会自动打到当前图片这一既有约定
   - **6.3 删除（已完成）**：`r` + `d` 先选预设（复用 6.2 的预设选择器），再从该预设下未删除的 version 里选一个（预设本身不出现在这一层，不是"选了拒绝"而是从一开始就不给选）。**跟 `handle_delete_tag_submenu` 刻意不同**：没有加一道 y/N 二次确认——标签删除是级联清光所有关联的不可逆项目级操作，这里只是软删除（从可选列表隐藏，不影响已经引用它的图片渲染），风险量级不对等，不需要同等重量的确认仪式
   - **6.4 收尾（已完成）**：退休了这个里程碑过程中新增的全部临时调试命令（`pzt recipe create-debug`/`apply-debug`/`clear-debug`、`pzt color-debug`），连带清理了只被它们使用的 `resolve_recipe_target`；保留 M1_PRD.md 定的正式命令（`pzt recipe list`/`rename`/`delete`，以及它们仍然依赖的 `find_preset_by_name`/`parse_recipe_address`/`resolve_recipe_address`），照抄 M0 increment 6.4.7 的收尾模式。`pzt open` 的 usage 提示补上了 `r` 键的说明
   - **验证方式的局限（写在这里,不是遗漏）**：`r` 键的全部交互逻辑只能在真实终端里手动验证（cbreak 模式下的按键读取没有可行的自动化测试路径，跟 M0 阶段全键盘循环的既有结论一致），这次没有新增可自动化的单元测试——已确认的是构建干净、既有 121 个单元测试全部通过（这次改动不涉及任何 core 层逻辑变化，纯 cli 接线）、退休的调试命令确实返回"未知子命令"、保留的命令确实还能正常工作
7. **导出烘焙（已完成）**：`export_tag` 内部按"这张图有没有应用 recipe"分两条路径——应用了的走解码→`recipe::render`（`hardware_concurrency()` 多线程）→`encode_jpeg_file`，替代直接拷贝/软链；没应用的完全不变。`--link` 对烘焙路径没有意义（输出本来就是新生成的文件），统一忽略 `link_mode` 落地成真实文件，混合导出时同一批图片里没应用的那些仍然正确遵守 `--link`。解码/渲染/编码任一步失败都复用现有的 `ExportSkipped` 跳过机制，不中断其余图片。`export_tag` 签名不变，`cli/main.cpp` 的 `cmd_export` 不需要任何改动。单元测试覆盖：应用了非恒等 recipe 的图片字节跟源文件不同、没应用的字节级逐字节相同（回归防线）、`--link` 混合场景两种图片各自的正确行为。真机验证：对 `Test` 项目里已经应用了 Warm/Origin 的真实照片跑一遍 `pzt export`，`shasum` 确认无风格图片字节不变，肉眼对比确认 Warm 图片体现了暖色效果
8. **集成与验收**：逐条核对 `docs/M1_PRD.md` 验收标准；用真实素材（而非合成测试图）试用一轮，重点看预览切换风格的主观卡顿感受

## 风险与待确认问题

延续自 `docs/M1_PRD.md`，这次工程设计阶段能定的都定了（version 的命令行寻址语法见 increment 2），以下几条仍然留到实现或真机验证阶段：

* **`recipe_versions` 参数的确切取值范围/裁剪规则**：高光/暗光/白平衡偏移目前只定了"是什么"，没定具体的数值范围和曲线形状（比如高光调整是线性增益还是某种 S 型曲线），increment 4 实现时需要定一个具体公式，先跑起来，后续可以调
* **内置预设的具体名称/数量/色彩倾向**：increment 1 只需要 1-2 个能验证机制的占位预设，真正的调色设计工作可以在 increment 1 完成后随时补充，不阻塞后续 increment
* **软删除 version 的清理时机**：M1_PRD.md 已经定为可选、不强制，这次不实现任何清理机制，如果后续发现孤儿 version 堆积成问题再补
* **软删除没有对应的"恢复"操作**：跟 `archive` 没有 `unarchive` 是同一类悬而未决状态
* **色彩管理（ICC）缺口维持 M0 假设**：不在 M1 处理，等真机测试发现明显偏色问题再评估
* **`--link` 导出遇到应用了 recipe 的图片时退化成真实文件拷贝**：这是这次工程设计阶段发现的、`--link` 语义的一个自然限制（见"core/api 接口设计"里 `core/export/` 修改一节），已经决定接受这个限制，不需要额外设计
