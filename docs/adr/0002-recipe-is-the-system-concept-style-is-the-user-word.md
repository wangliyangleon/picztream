# 配方是系统概念,风格是用户的词,两者之间是一条翻译边界

**Status**: accepted

同一件事在本仓库里有两个名字:`core`/`cli` 一律叫 **recipe / 配方**,`agent` 一律叫 **Style**。这不是术语分叉,而是一条真实的语义边界,**刻意保留**:

- **配方(recipe)** 是系统里精确、可执行的那个对象 - 两层模型(内置预设 + 用户在预设之上存下的自定义配方),有 id、有 8 个数值旋钮、能被 `set_image_recipe` 直接写进 `images.recipe_id`。坐在终端前按 `r` 的人做的是"在 10 个预设里选第 3 个、再选自己存的那一版",他选的是一个**精确对象**。
- **风格(style)** 是用户口语里"我想要什么感觉"的表达 - "胶片感"、"清爽一点"、"跟上次那种一样"。它模糊、不可直接执行,必须先被解析。

两者之间的翻译器已经在代码里了:`agent/compose/style_matcher.py::match_style_description` 吃一段自由文本、吐一个 `recipe_name`。`Style` Stage 叫 Style 而调的是 `pzt recipe apply`,正是这条边界的现成体现。

这条边界落在 `SPEC.md` §2.3 那条轴上:**关于用户的推理归 agent**,所以"用户嘴里的风格"归 agent;精确的、可执行的那个东西归 `core`/`cli`,那就是配方。本 ADR 是那条轴的一个具体实例。

## Considered Options

- **统一成 style**(否决):把 `core/recipe/` 改名 `core/style/`、`RecipeId` → `StyleId`、`recipes` 表 → `styles`、headless `pzt recipe apply` → `pzt style apply`。实测规模 ~960 处(C++ 676 / agent 78 / docs 206,涉及 38 个 C++ 文件),且撞三处硬伤:`core/ai/style.h` 已存在(AI 看图选风格),"style" 这个词在仓库里已被"选风格这个动作"占了一半;headless 命令改名会让 brew 装的旧 `pzt` 配新 agent 从"报错"退化成"命令不存在"(同类真机坑 `AGENTS.md` 已记过一次);`recipes` 表与 `images.recipe_id` 改名要动用户已有的库,而本仓库的迁移机制 `ensure_column` 是纯增量的,表改名没有先例。
- **统一成 recipe**(否决):agent 侧改口叫 recipe。代价是把"用户说的那句模糊的话"和"系统里那个精确对象"压成同一个词,而 agent 的全部工作恰恰是**区分**这两者 - `style_matcher` 的输入输出会变成 recipe → recipe,读者看不出中间发生了翻译。
- **保留两个词,把边界写下来**(采纳):零代码改动,代价是要向未来的读者解释为什么同一件事有两个名字 - 本 ADR 就是那个解释。

## Consequences

- `cli` 的显示文案统一用"配方"(此前"风格" 3 处 / "配方" 1 处并存),控制台批量命令叫 `/recipe`、快捷键仍是 `r`。
- 跨层读代码时,看到 `chosen_recipe`(agent 侧,`Style` Stage 的输出)不要以为是命名不一致 - 那正好是翻译**已经发生**的那个点:输入是 style(自由文本),输出是 recipe(精确名字)。
- 未来若新增"按口语描述套配方"的交互层能力(今天只有 agent 有),它必须先经过一次翻译才能落到 `set_image_recipe`,而那次翻译按 §2.3 归 agent - 也就是说这条能力不能只在 `cli` 里做完。
