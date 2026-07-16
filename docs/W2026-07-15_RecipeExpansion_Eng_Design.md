# 目标二第一刀 Eng Design：Recipe 预设系统扩展（City+Year 一级风格）

## 一、背景

`docs/W2026-07-15_PRD.md` 目标二（Recipe/滤镜系统扩展）拍板了一套"城市+年份"命名的预设体系：6 个彩色 + 3 个黑白，映射键盘 1-9，键 0 固定是 `Origin`（无需改动，`r+0`/`r+r` 已经直接走 `recipe_id=NULL`）。

`core/recipe/recipe.cpp::ensure_default_presets()` 目前只播种两个预设：固定 id=0 的 `Origin`（无 LUT）和一个占位的 `Warm`（`make_warm_lut`，随手调的暖色偏移，注释里明确写着"只用来验证机制通不通，真正的调色设计留到后面"）。这次把 `Warm` 换成真正设计过的 9 个预设。

调研过程中确认了两个之前遗留、这次一起解决的缺口：

1. `cli/menu/recipe_menu.cpp::presets_for_menu()` 目前把 `Origin` 也计入按创建顺序编号 1-9 的列表——`Origin` 固定 id=0 且总是第一个播种，会一直占用键位 `1`。这是 `recipe.h` 里本来就留着的一条"**待确认**"TODO，这次落地时一并解决。
2. 3D LUT（`core/color::Lut3D`）是逐像素独立的颜色映射，能完整表达白平衡、饱和度、对比度、整体明暗，但无法表达颗粒（film grain）——颗粒是空间噪声，不是颜色映射能力。这次新增一个独立的颗粒合成 pass。

## 二、数据安全性：清理 `Warm`

`images.recipe_id` 是 `INTEGER REFERENCES recipes(id) ON DELETE SET NULL`；`recipes.parent_id` 是 `INTEGER REFERENCES recipes(id) ON DELETE CASCADE`；`core/db/schema.cpp::initialize_schema` 开头执行 `PRAGMA foreign_keys = ON`，全局生效。

因此对 `Warm` 预设行执行 `DELETE FROM recipes WHERE name='Warm' AND parent_id IS NULL` 是完全安全的：

- 级联删除它名下的所有 version（`parent_id` 外键的 `ON DELETE CASCADE`）。
- 任何引用过 `Warm`（预设本身或其 version）的图片，`images.recipe_id` 会被 SQLite 自动置为 `NULL`（`ON DELETE SET NULL`），效果等同于回退到 `Origin` 直出。

不需要新增预设级软删除机制。用户真实库（`~/.config/pzt/pzt.db`）核实过有 4 张图片（id 15/18/27/72）引用了 `Warm` 或其 version，清理后这些图片会变成"未应用风格"，这是"清理掉现有滤镜"这个请求的应有效果。

## 三、`GradeParams` / `make_graded_lut`：把数值旋钮烘焙成 LUT

`core/recipe/recipe.h` 的 `namespace detail`（比照 `core::ai::detail::downscale_for_upload` 的先例：仅供单元测试和模块内部使用，不进 `core/api` 门面）新增：

```cpp
struct GradeParams {
  double wb_shift_r = 0;   // -100..100,同 apply_adjustments 的白平衡通道增益
  double wb_shift_b = 0;
  double saturation = 0;   // -100..100,-100 时天然坍缩成黑白
  double contrast = 0;     // -100..100,以 0.5 为中点的线性缩放
  double brightness = 0;   // -100..100,整体加成(不分高光/暗部,那是 version 的事)
};

std::vector<float> make_graded_lut(int n, const GradeParams& params);
```

对每个网格点 `(r,g,b) ∈ [0,1]³` 依次做四步：

1. **白平衡**：`r1 = r*(1+wb_shift_r/100)`，`g1 = g`，`b1 = b*(1+wb_shift_b/100)`。
2. **整体明暗**：三通道各自加 `brightness/100`。
3. **对比度**：以 0.5 为中点线性缩放，`c3 = (c2-0.5)*(1+contrast/100)+0.5`，三通道各自做。
4. **饱和度**：`luma = 0.299*r3+0.587*g3+0.114*b3`（用第 3 步之后的值算），`c4 = luma + (c3-luma)*(1+saturation/100)`。

最后 clamp 到 `[0,1]`。**`saturation=-100` 时第 4 步恒等于 `luma`，三通道相等，天然产出黑白效果**——黑白预设直接复用同一个函数，不需要独立代码路径。

这套公式刻意跟 `core/color::apply_adjustments` 的白平衡模型（`R'=R*(1+wb_shift_r/100)+delta`）保持同一套语言，方便对照，但两者语义不同：`apply_adjustments` 是运行时按每个 version 实时调整的参数，`make_graded_lut` 是烘焙进 `recipes.base_lut` 的静态查找表，调用一次生成后不再变。

## 四、颗粒：`recipes.grain_amount` + `color::apply_grain`

### 4.1 Schema

`core/db/schema.cpp::initialize_schema` 追加一条 `ensure_column`（照抄 `support_raw` 列"旧库迁移落在默认值、语义不变"的先例）：

```sql
ALTER TABLE recipes ADD COLUMN grain_amount REAL NOT NULL DEFAULT 0;
```

取值范围 `0..1`。跟 `base_lut`/`base_lut_size` 一样是**预设级烘焙好的"底子"**，`VersionParams` 不能覆盖它——version 只能调 `highlights`/`shadows`/`wb_shift_r`/`wb_shift_b` 这四个细节参数。

### 4.2 `ResolvedRecipe`

```cpp
struct ResolvedRecipe {
  std::optional<color::Lut3D> lut;
  VersionParams params;
  double grain_amount = 0;  // 来自预设行,version 不能覆盖
};
```

`resolve_recipe` 内部把原来只读 `base_lut`/`base_lut_size` 的 `load_lut` 扩展成 `load_preset_look`，同一次查询里一起读出 `grain_amount`。

### 4.3 `core::color::apply_grain`

```cpp
void apply_grain(decode::DecodedImage& img, float amount, unsigned thread_count = 1);
```

算法：纯位置哈希生成单色噪声，不依赖图片内容或时间戳做种子——**同一张图、同一个 recipe，重复渲染得到完全相同的颗粒图案**，预览滚动/prefetch 缓存复用时不会看起来"闪烁"。

```cpp
inline float grain_noise(int x, int y) {
  std::uint32_t h = x * 374761393u + y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= (h >> 16);
  return (float(h & 0xFFFFu) / 65535.f) * 2.f - 1.f;  // [-1, 1]
}
```

三通道加同一个偏移量（单色噪声，更接近真实胶片颗粒对亮度而非色相的影响），幅度 = `amount * kGrainMaxIntensity`，`kGrainMaxIntensity = 0.12`（约 30/255，`amount=1.0` 时的最大加减幅度）。这是第一版工作假设，不是摄影级精确校准，真机验证后可以调整常量。

跟 `apply_lut`/`apply_adjustments` 共用同一个 `run_parallel_rows` 行切分帮手，逐行独立、`thread_count>1` 时天然线程安全。

### 4.4 接入 `render`

```cpp
if (resolved->grain_amount > 0) {
  color::apply_grain(out, static_cast<float>(resolved->grain_amount), thread_count);
}
```

`grain_amount<=0` 时完全跳过，跟"Origin 没有 LUT 就跳过 apply_lut"是同一个已有的优化精神——不做无意义的整图遍历。

## 五、9 个内置预设的数值表

`wb_shift_r`/`wb_shift_b`/`saturation`/`contrast`/`brightness` 是 `-100..100` 的百分比，`grain` 是 `0..1`。键位=插入顺序（`list_presets()` 按 `id ASC` 排序，等价于创建顺序）。LUT 网格用 `n=17`，跟原来的 `Warm` 一致。

| 键 | 名字 | wb_r | wb_b | saturation | contrast | brightness | grain | 说明 |
|---|------|------|------|-----------|----------|------------|-------|------|
| 1 | Havana 1959 | +15 | -10 | +35 | +10 | +5 | 0.25 | 暖调、高饱和、古巴风情 |
| 2 | Tokyo 1966 | +12 | -8 | -25 | -15 | +8 | 0.25 | 昭和时代暖调、低饱和、柔亮怀旧 |
| 3 | Paris 1974 | +10 | -6 | -15 | -15 | +5 | 0.25 | 暖调、中低饱和、低对比 |
| 4 | Miami 1986 | 0 | 0 | +40 | +25 | 0 | 0.25 | 中性白平衡、高饱和高对比、glossy |
| 5 | New York 1994 | -8 | +10 | -30 | +20 | -5 | 0.55 | 冷调、低饱和、高对比、粗颗粒 |
| 6 | Shanghai 2010 | -5 | +5 | +35 | +18 | +3 | 0.10 | 中冷调、高饱和、glossy 商业感、极细颗粒 |
| 7 | Munich 1951 | 0 | 0 | -100 | +45 | -5 | 0.55 | 黑白，极高对比、深邃影调、粗颗粒 |
| 8 | Rome 1960 | 0 | 0 | -100 | -10 | +10 | 0.25 | 黑白，中低对比、柔亮影调（La Dolce Vita） |
| 9 | Berlin 1989 | 0 | 0 | -100 | +20 | -8 | 0.55 | 黑白，高对比、偏暗、情绪紧迫、粗颗粒 |

**这些数值是第一版工作假设**（跟 `apply_adjustments` 文档注释"先保证功能正确、可测试"、以及目标一 `gemma4:e2b` 默认模型选定前先做真机基准测试是同一个"先落地可测试的版本、再用真机效果校准"的精神），实现完成后在真机上对着真实照片逐个检查，观感不对就直接改 `core/recipe/recipe.cpp::builtin_presets()` 里的数字重新提交，不涉及架构变化。

## 六、`presets_for_menu()` 过滤 Origin

`cli/menu/recipe_menu.cpp::presets_for_menu()` 现在直接把 `list_presets()`（含 `Origin`）截断到 9 个,导致 `Origin` 占用键位 `1`。修复后按 `id==0` 过滤掉 `Origin`（`Origin` 的 id 固定为 0，见 `seed_origin_preset`），再截断到 9：

```cpp
std::vector<pzt::core::PresetSummary> presets_for_menu() {
  auto presets = pzt::core::list_presets();
  std::vector<pzt::core::PresetSummary> numbered;
  for (auto& p : presets) {
    if (p.id == 0) continue;
    numbered.push_back(std::move(p));
  }
  if (numbered.size() > 9) numbered.resize(9);
  return numbered;
}
```

`r+0`/`r+r` 快捷清除路径不受影响——它们一直是独立于预设列表、直接把 `recipe_id` 设成 `NULL` 的快捷路径，不经过这个函数。

## 七、任务分解（第一刀：内置预设）

1. 本文档。
2. `core::color::apply_grain`。
3. `detail::GradeParams`/`detail::make_graded_lut`。
4. `recipes.grain_amount` 列 + 接入 `resolve_recipe`/`render`。
5. 清理 `Warm`，播种 9 个 City+Year 预设。
6. `presets_for_menu()` 过滤 Origin。
7. 真机验证，按观感微调数值表。

第一刀具体步骤见 `.claude/plans/fully-understand-the-pzt-iterative-gosling.md`（该文件已被第二刀的计划覆盖，历史步骤见 git 提交记录 `196841d`..`3c76572`）。

## 八、用户自建 version 旋钮扩展（第二刀）

第一刀落地了 9 个内置预设（`base_lut`/`grain_amount`，预设级烘焙好，用户不能改）。这一刀轮到用户在预设基础上自建的 `version`——`VersionParams` 原本只有 `highlights`/`shadows`/`wb_shift_r`/`wb_shift_b` 四个可调旋钮（increment 2 时代"先落地的最小集合"），这次加 **对比度、饱和度、黑色、白色** 四个新旋钮，白平衡维持现状（`wb_shift_r`/`wb_shift_b` 双通道独立增益模型，不换成单一色温滑块——两个通道各自独立增益能表达的范围比单一色温滑块更宽，且改动成本为零）。

### 8.1 `core::color::AdjustParams`

`apply_adjustments` 原本是 4 个 `double` 位置参数，加到 8 个会变成容易传错顺序的陷阱——照抄第一刀 `detail::GradeParams` 遇到同样问题时的解法，换成一个结构体：

```cpp
struct AdjustParams {
  double highlights = 0;
  double shadows = 0;
  double blacks = 0;
  double whites = 0;
  double wb_shift_r = 0;
  double wb_shift_b = 0;
  double contrast = 0;
  double saturation = 0;
};
void apply_adjustments(decode::DecodedImage& img, const AdjustParams& params,
                        unsigned thread_count = 1);
```

`core/color` 依然不知道"version"是什么，`AdjustParams` 只是一捆颜色调整参数，不是 `core::recipe::VersionParams` 本身——`core::recipe::render()` 负责做两者之间的映射，不让 `core/color` 反向依赖 `core/recipe`。

### 8.2 公式

顺序跟 `make_graded_lut` 保持一致：白平衡增益 → 整体明暗 → 对比度 → 饱和度，四步全在同一次逐像素遍历里做完，不新增遍历趟数：

```
luminance = 0.299R + 0.587G + 0.114B（原始输入像素）
highlight_weight = clamp((luminance-0.5)/0.5, 0, 1)   // 半程,0.5→1 渐强
shadow_weight    = clamp((0.5-luminance)/0.5, 0, 1)   // 半程,0.5→0 渐强
white_weight     = clamp((luminance-0.8)/0.2, 0, 1)   // 窄带,只在 0.8→1 渐强
black_weight     = clamp((0.2-luminance)/0.2, 0, 1)   // 窄带,只在 0→0.2 渐强
brightness_delta = highlights/100*highlight_weight + shadows/100*shadow_weight
                  + whites/100*white_weight + blacks/100*black_weight
R1 = clamp(R*(1+wb_shift_r/100) + brightness_delta, 0, 1)
G1 = clamp(G + brightness_delta, 0, 1)
B1 = clamp(B*(1+wb_shift_b/100) + brightness_delta, 0, 1)
contrast_gain = 1 + contrast/100
R2 = clamp((R1-0.5)*contrast_gain + 0.5, 0, 1)   (G2/B2 同理)
luma2 = 0.299*R2 + 0.587*G2 + 0.114*B2
sat_gain = 1 + saturation/100
R3 = clamp(luma2 + (R2-luma2)*sat_gain, 0, 1)    (G3/B3 同理)
```

黑色/白色特意用比 highlights/shadows 更窄的加权区间（0-0.2 / 0.8-1，而不是 highlights/shadows 的 0-0.5 / 0.5-1 整个半程）——这是摄影里"影调端点"和"影调恢复"两个不同概念的常规区分，不这样区分的话黑色/白色会跟暗光/高光在效果上几乎重叠，加了也没有新意义。0.2/0.8 这两个阈值是这次实现时定的工作假设，不是精确调出来的数字，真机验证后可以调整。对比度/饱和度的公式直接照抄 `make_graded_lut`，只是从"烘焙进静态 LUT"变成"每次渲染时对 `R1/G1/B1` 再做一遍"。

### 8.3 数据层

`recipes` 表新增 4 列，走 `ensure_column`（照抄 `grain_amount` 那次）：`contrast`/`saturation`/`blacks`/`whites`，都是 `REAL NOT NULL DEFAULT 0`。旧库里已有的 version 行迁移后这 4 个新旋钮自动落在 0（中性，不影响现有效果）。

`VersionParams`/`VersionSummary` 新增 4 个字段追加在结构体末尾（不插入中间、不用 designated initializer）——现存的位置初始化 `VersionParams{a,b,c,d}` 依赖字段声明顺序不变，新增字段追加在末尾且都有默认值 `=0`，C++20 聚合初始化对未显式给出的尾部字段用默认成员初始化值填充，不需要跟着改。

### 8.4 CLI

交互维持现有"一个字段一个 prompt"的模式（`cli/menu/recipe_menu.cpp::handle_r_create_flow`），不改造成一次性多值输入——这是当前唯一的既有模式，改交互形式是超出这次范围的独立设计决定。新增 4 个 prompt 直接照抄 `recipe_menu_input_highlights` 等 4 个已有 i18n 函数的写法（zh/en 双语分支）。`pzt recipe list` 的展示格式（`msg_recipe_version_item`）同步加 4 个数值。

### 8.5 任务分解

1. 本节文档。
2. `core::color::AdjustParams` + `apply_adjustments` 扩展公式。
3. `recipes` 表新增 4 列 + `VersionParams`/`resolve_recipe`/`render` 接入。
4. CLI 接入——创建流程 4 个新 prompt + `pzt recipe list` 展示。
5. 真机验证。

具体步骤见 `.claude/plans/fully-understand-the-pzt-iterative-gosling.md`（本次实现用的 TDD 计划）。
