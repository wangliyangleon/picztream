# 色彩流水线性能 Spike(M1 Phase 0)

一次性验证探针,不是生产代码,回答 `docs/Roadmap.md` Milestone 1 描述里两个没有实测数据支撑的设计假设:

1. "降采样同步预览" 的色彩处理是否真的能在按键触发的同步延迟预算内完成(沿用 M0 定的 100ms 目标)?
2. "全分辨率处理走异步队列" 是否真的有必要,还是实测发现全分辨率也很快、这层设计复杂度是不必要的?

顺带回答一个容易想当然的问题:Roadmap 提到 ARM NEON SIMD,但 3D LUT 三线性插值是"按像素颜色决定读哪里"的查表操作,NEON 没有好用的通用 gather 指令,不一定是 SIMD 的强项——这次验证同时测了 `std::jthread` 多线程是否已经够用,而不是假设"慢就上 NEON"。

结论见 `results.md`,会在 `docs/M1_Eng_Design.md`(还没写)里被引用。

## 构建与运行

```
clang++ -std=c++20 -O2 -fexperimental-library -o probe probe.cpp \
  -framework CoreGraphics -framework ImageIO -framework CoreFoundation
./probe <jpeg1> [jpeg2 ...]
```

跟 `core/CMakeLists.txt` 的其它目标一样需要 `-fexperimental-library`(`std::jthread` 在这套 Apple Clang 工具链上的已知要求,见 `docs/M0_Eng_Design.md`)。

依赖 macOS 自带的 ImageIO/CoreGraphics 做 JPEG 解码,跟 `core/decode`、`kitty_latency_probe` 用的是同一条路径,这里独立抄一份而不是链接 `core` 静态库,保持这个 spike 完全自包含、跟 CMake 构建链无关(跟 `kitty_latency_probe` 同样的惯例)。

## 测试素材

用一张真实照片(`~/Pictures/pzt_test/PXL_20260628_051145621.jpg`,3072x4080,只读引用,没有修改这个已有项目)通过 `sips` 生成五档合成样本,不提交进仓库(`.gitignore` 排除了 `*.jpg`,内容衍生自真实照片,体积也不小):

| 档位 | 分辨率 | 像素数 | 用途 |
|---|---|---|---|
| preview_small | 1280x854 | 1.09MP | 终端面板典型预览尺寸(偏小) |
| preview_large | 1920x1280 | 2.46MP | 终端面板典型预览尺寸(偏大) |
| full_12mp | 4000x3000 | 12MP | 跟 `kitty_latency_probe` 的 12MP 档对齐,方便对照 |
| full_24mp | 6000x4000 | 24MP | 同上,24MP 档 |
| full_60mp | 10000x8000 | 80MP(命名沿用 `kitty_latency_probe` 的"60MP 档"叫法,实际按面积算是 80MP) | 同上,高像素机型档 |

## 测的是什么

两种色彩操作,各测单线程 scalar 基线和 `std::jthread` 按行切分多线程(线程数 = `hardware_concurrency()`)两个版本,每个配置跑 5 次取最快一次:

- **白平衡**:逐像素 R/G/B 各乘一个增益系数,`O(1)` 次乘加,作为"简单变换"的对照组。
- **3D LUT 三线性插值**:17³ 和 33³ 两档网格大小(常见调色 LUT 规格),LUT 内容是一个有实际扭曲的合成配方(轻微 S 型对比度 + 暖冷偏移),不是恒等映射——恒等映射会让 CPU 分支预测器/预取器"猜对"太多次,测出来的耗时会比真实调色 LUT 偏乐观。

没有写 NEON intrinsics:先看 scalar + `jthread` 是否已经够用,符合 `CLAUDE.md` "不做过早优化,SIMD 仅在有实测数据支撑时引入"这条工程契约。

## 已知局限

跟 `kitty_latency_probe` 一样,这次探针的运行环境(Bash 工具)本身不影响这次测的内容——这次纯粹是 CPU 计算耗时,不涉及 pty/终端 I/O,所以不存在"非真实 tty 导致数据不可信"这个局限。唯一的局限是:合成 LUT 是人为构造的扭曲映射,不代表所有可能的真实调色配方在数值分布上的行为,但已经避免了恒等映射这个最容易导致虚高结果的陷阱。
