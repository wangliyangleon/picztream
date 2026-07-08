# PicZTream (PZT) Milestone 2 工程设计文档

## 背景

`docs/M2_PRD.md` 已经拍板了 M2 的产品决策(纯 DNG/RAF、按需触发 LibRaw 解码、8-bit sRGB 输出),按 `AGENTS.md` 的工程契约,具体的表结构、模块划分、接口签名要落到这份文档,implementation 应在本文档评审通过后再开始。

**这份文档经历过一次方向调整**(increment 3/4 实现完、真机验证时发现的问题倒逼的):最初设计是"RAW+JPEG 同名配对,配对图片预览用 JPEG 伴侣文件、导出才碰 RAW",跑通后真机测试发现——某些相机(比如用户的徕卡 Q3)在黑白胶片模式下拍摄时,内嵌/伴侣 JPEG 是黑白的,但 LibRaw 解码 RAW 数据永远是彩色,导致预览和导出观感割裂,应用 recipe 时这个落差还会被放大(白平衡偏移对灰阶图会直接染色)。改成现在这版:**不再保留配对,同名 JPEG 直接忽略;预览也走 LibRaw 解码(降分辨率),在 `new`/`rescan` 时一次性生成缓存**,换取预览和导出色彩一致,代价是 `new`/`rescan` 处理 RAW 文件时需要付出一次性解码成本、需要进度提示。以下内容是调整后的最终设计,不再保留旧版配对方案的细节(git 历史里能查到)。

## LibRaw 解码性能验证结论

对应 M2_PRD.md"技术方案概要"提到的性能未知项,已经用一个独立探针(`spikes/libraw_probe/`)对用户本地真实拍摄的素材验证过,完整数据见该目录下的 `results.md`,这里摘要对设计有直接影响的结论:

1. **内嵌预览提取(`unpack_thumb` + `dcraw_make_mem_thumb`)远低于 100ms 延迟预算**:两台真实机身(徕卡 Q3、富士 X-T5)都在 3ms 以内完成,culling 路径"不触发全量解码"的核心假设成立。
2. **两台机身的内嵌预览格式都是 `LIBRAW_IMAGE_JPEG`**,不是位图,`extract_embedded_jpeg_bytes` 不需要实现位图兜底分支。
3. **富士 X-Trans 全量解码比徕卡拜耳阵列慢 2.4-2.6 倍**(换算每兆像素耗时,X-Trans 约是拜耳的 4 倍),符合"X-Trans 非规则阵列插值窗口更大"的预期,这次有了具体数字:徕卡约 2.0-2.2 秒/张(60MP),富士约 5.3 秒/张(40MP)。
4. **全量解码是秒级耗时,`pzt export` 需要为需要真正解码的 RAW 图片加进度提示**,这是 spike 之前 PRD 风险清单里悬而未决的问题,现在有数据支撑要不要做——静默跑几十张、每张几秒,总耗时到分钟级,不给任何反馈体验会很差。
5. **不需要为全量解码额外引入并发设计**:LibRaw 内部已经用 OpenMP 并行,`pzt export` 沿用 M1"每张图片各自处理,不需要常驻后台队列"的模型即可。

### 对 core 设计的直接影响

- 预览路径:`core::raw::extract_embedded_jpeg_bytes` 只处理 JPEG 类型,不做位图分支(真机测试没遇到过再加);这个函数现在降级成"预览缓存缺失时的兜底路径",不是主路径(见下"RAW 预览缓存"一节)
- 处理路径:`core::raw::decode_full`(全量解码,导出用)和新增的 `core::raw::decode_preview`(half_size 降分辨率解码,预览缓存生成用)都不需要 `jthread`/线程池包装,直接调用 LibRaw 单线程 API(LibRaw 内部自己用 OpenMP)
- `pzt export` 遇到需要走 `raw::decode_full` 的图片时,往 stderr 打一行"正在处理 X/N"的轻量进度日志;`pzt new`/`pzt rescan` 遇到需要生成预览缓存的 RAW 图片时同样需要进度提示,见"RAW 预览缓存"一节

## 数据库 Schema 设计

`images` 表新增两列(复用 M1 已经建立的 `ensure_column` 幂等迁移机制):

```sql
ALTER TABLE images ADD COLUMN kind TEXT NOT NULL DEFAULT 'jpeg';
-- 'jpeg' | 'raw'。两态，不再有 'raw_jpeg'——同名 JPEG 存在时直接忽略，不
-- 生成配对记录。M0/M1 时代的旧库迁移时所有已有行都落在默认值 'jpeg'。

ALTER TABLE images ADD COLUMN preview_cache_path TEXT;
-- kind='raw' 时才可能有值：new/rescan 时用 LibRaw half_size 模式生成的降
-- 分辨率预览 JPEG 的绝对路径，落在 PZT 自己的数据目录下（不在用户照片文
-- 件夹内，见"RAW 预览缓存"一节）。生成失败、或还没来得及生成时为 NULL，
-- 这种情况下预览退化到内嵌预览提取兜底路径。
```

`file_path`/`file_name` 两列的语义保持跟 M0/M1 完全一致——就是这张图对应的那个文件的相对路径，`kind='raw'` 时就是 `.dng`/`.raf` 本身，不再有"配对时改存 JPEG 路径"这种特殊情况。`preview_cache_path` 是绝对路径（不是相对 `root_path`，因为它压根不在 `root_path` 目录树下）。

不新增唯一索引约束——不需要跨行约束。

## core/api 接口设计

### `core/raw/`(新模块,纯 LibRaw 封装,不碰数据库,不碰 ImageIO)

```cpp
namespace pzt::core::raw {

enum class RawError {
  FileNotFound,
  DecodeFailed,  // 文件读到了，但 LibRaw 打不开/解不出来，或内嵌预览不是 JPEG 格式
};

// unpack_thumb() + dcraw_make_mem_thumb()，只返回内嵌预览的原始 JPEG 字节，
// 不解码成像素——像素解码复用 core::decode 已有的逻辑（见下）。type 不是
// LIBRAW_IMAGE_JPEG（极少数机型内嵌位图缩略图）时归为 DecodeFailed；spike
// 已确认徕卡/富士两种测试机型都是 JPEG 类型，M2 不为位图分支投入实现，
// 真机测试遇到例外再处理。
Result<std::vector<std::uint8_t>, RawError> extract_embedded_jpeg_bytes(const std::string& path);

// open_file -> unpack -> dcraw_process(output_bps=8, use_camera_wb=1,
// output_color=1/*sRGB*/) -> dcraw_make_mem_image。LibRaw 内部完成白平衡
// （as-shot）+ 去马赛克 + 色彩矩阵 + gamma，直接吐 8-bit sRGB，转换成
// core::decode::DecodedImage 的 RGBA 字节布局（RGB 三通道 + 1 字节跳过，
// 对齐 core/decode/decode.cpp 用的 kCGImageAlphaNoneSkipLast +
// kCGBitmapByteOrder32Big 约定）直接返回，供 core::recipe::render 无缝
// 消费，core::color 不需要任何改动。
Result<decode::DecodedImage, RawError> decode_full(const std::string& path);

// unpack -> dcraw_process(跟 decode_full 同样的 output_bps=8/use_camera_wb=1/
// output_color=1，额外加 half_size=1) -> dcraw_make_mem_image。跳过完整去
// 马赛克，直接 2x2/3x3 块平均，真机实测徕卡 ~944ms/富士 ~120ms(全量解码
// 分别是 ~1998ms/~4972ms)，富士这边几乎是把最贵的 X-Trans 去马赛克整个跳
// 过了。分辨率减半(徕卡 4768x3172、富士 3876x2589)，白平衡/色彩矩阵/
// gamma 这条管线跟 decode_full 完全一样，只是去马赛克步骤更便宜——这是
// "预览色彩要跟导出一致"这个目标能用低成本实现的关键：不是另开一条渲染
// 管线，只是同一条管线在更低分辨率下跑一遍。只给 new/rescan 生成预览缓存
// 用，不能拿来当导出结果（分辨率不够）。
Result<decode::DecodedImage, RawError> decode_preview(const std::string& path);

}  // namespace pzt::core::raw
```

`decode_full`/`decode_preview` 的实现要点:`libraw_processed_image_t` 返回的是 3 字节/像素紧凑排列(`colors=3, bits=8`),转换成 `DecodedImage` 的 4 字节/像素(RGBA,alpha 位跳过)布局需要一次逐像素展开拷贝,不是简单的 `memcpy`。这一步以及最终的字节序/通道顺序要不要跟现有 `decode_jpeg_file` 严格对齐,是实现阶段第一个要写单元测试(用一个已知 RGB 值的小图或者直接跟真机验收的肉眼对比)锁定的地方,避免颜色通道错位这种"能跑但是错的"问题(呼应 PRD 风险清单里"色彩管理边界变化"那条)。

### `core/decode/` 增补

把现有 `decode_jpeg_file` 拆成两半,新增一个复用点:

```cpp
// 已有 decode_jpeg_file(path) 内部改成: read_file(path) 后调这个新函数。
Result<DecodedImage, DecodeError> decode_jpeg_bytes(const std::vector<std::uint8_t>& bytes);
```

`raw::extract_embedded_jpeg_bytes` 拿到内嵌预览的字节后,直接调 `decode_jpeg_bytes`,不重新实现一遍 CGImageSource 逻辑。`core/decode` 不新增任何对 LibRaw 的依赖,`core/raw` 也不依赖 ImageIO——两个模块保持单一职责、互不依赖。

### 预览调度门面函数(`core/api.cpp`,不新增头文件)

```cpp
// 按扩展名分发:.jpg/.jpeg 走 decode::decode_jpeg_file(不变)；.dng/.raf
// 走 raw::extract_embedded_jpeg_bytes + decode::decode_jpeg_bytes。这是
// core/decode 和 core/raw 之间唯一的"胶水"逻辑，放在 api.cpp 而不是塞进
// 其中任何一个模块，保持两者互不依赖。
Result<DecodedImage, DecodeError> decode_preview_file(const std::string& path);
```

`cli/commands/browse.cpp:136` 现有的 `PrefetchCache prefetch(project.root_path, 3, pzt::core::decode_jpeg_file);` 改成传 `pzt::core::decode_preview_file`。`PrefetchCache`/`browse::DecodeFn` 本身的类型签名不需要任何改动——这次验证了 M1 时期这个类设计成"解码函数可注入"的决定是对的,新的解码策略接入零成本。

### `core::project` 增补:格式识别(不再做配对)

```cpp
// project.cpp 内部，is_jpeg 旁边新增:
bool is_raw(const fs::path& p);  // 扩展名在 {.dng, .raf} 内，大小写不敏感

// ScannedImage 只增加一个字段(不再需要 raw_relative_path)：
struct ScannedImage {
  std::string relative_path;
  std::string file_name;
  std::int64_t file_size;
  std::string kind;  // "jpeg" | "raw"
};
```

`scan_jpegs` 改名 `scan_media`,先各自扫出 JPEG 类和 RAW 类两个文件列表,按"所在目录 + 文件名主干(不含扩展名,大小写敏感比较主干本身)"分组:

- 一组里存在 RAW 文件(不管有没有同名 JPEG)→ 只产出一条 `kind="raw"` 记录,`relative_path` 是 RAW 自己的路径;同名 JPEG(如果有)直接忽略,不产生任何记录
- 一组里只有 JPEG,没有 RAW → `kind="jpeg"`(M0/M1 现有行为,不变)
- 同一主干出现多个 RAW 文件(不该发生,但不强行消歧)→ 每个各自独立成一条 `kind="raw"` 记录

`is_raw` 用一个 `constexpr` 字符串数组表达受支持的扩展名集合,不写死在多处判断里,给"未来加 CR2/CR3/NEF/ARW"这个已知的未来考虑留好扩展点。

### RAW 预览缓存:生成、存放、生命周期

这是这次方向调整新增的核心机制。`create_project`/`rescan_project` 对每一条新增或刚被识别成 `kind="raw"` 的记录,在写入 `images` 表之后,调用 `raw::decode_preview` + `decode::encode_jpeg_file` 生成一份降分辨率预览 JPEG,写入 PZT 自己的数据目录,再把这个绝对路径写回该行的 `preview_cache_path` 列。

**存放路径**:`<xdg_config_home>/pzt/raw_previews/<project_id>/<image_id>.jpg`——复用 `core/db/database.cpp` 里已经算好的 `default_db_path()` 所在目录(同一个 `~/.config/pzt/` 下开一个兄弟子目录,不是新的配置根),按 `project_id` 分子目录方便删项目时整目录删除。`database.h`/`.cpp` 补一个 `raw_preview_cache_dir(ProjectId)` 辅助函数返回这个路径,`core::project` 和 `core::db` 之外的模块不需要知道具体存放规则。

**生成失败时**:保持 `preview_cache_path` 为 NULL,不阻断这次 `new`/`rescan` 的其它文件(照抄导出路径"单张失败不中断整体"的既有惯例),浏览时该图退化到 `raw::extract_embedded_jpeg_bytes` 兜底路径(见 PRD"浏览与选片"一节)。

**进度回调**:`create_project`/`rescan_project` 都新增一个可选参数:

```cpp
using ScanProgressFn = std::function<void(int done, int total)>;
```

只有当这次扫描/rescan 里确实有需要生成缓存的 RAW 图片时才会被调用(纯 JPEG 项目、或者本来就有缓存不需要重新生成的 RAW 图片,不触发回调,不产生"进度是 0/0"这种没有意义的输出)。`cli` 侧传一个用 `\r` 覆盖同一行打印"正在生成 RAW 预览缓存 (X/N)..."的 lambda,`total` 已知(等于这次要生成缓存的 RAW 图片数量),不是那种边扫描边发现总数变化的场景,不需要处理"进度条总数中途变化"这种复杂情况。

**生命周期**:`rescan_project` 的 prune 分支删除一条记录之前,如果它的 `preview_cache_path` 非空,先 `fs::remove` 掉那个缓存文件,再删数据库行(避免留下没有对应数据库记录的孤儿缓存文件)。`delete_project` 整个删除一个项目时,`fs::remove_all(raw_preview_cache_dir(project_id))` 把这个项目名下所有缓存文件连同子目录一起清掉——这不违反"不触碰用户原始图片文件"的承诺,缓存目录压根不在用户照片文件夹里,是 PZT 自己的数据。

### `core::project::rescan_project` 的升级逻辑(单方向,已简化)

去掉配对之后,只剩一种需要"原地升级而不是插入新记录"的场景:项目建的时候文件夹里只有 JPEG(`kind="jpeg"`),之后才把同名的 RAW 拷贝进来。这种情况下,已有的 `kind="jpeg"` 记录要被 **UPDATE** 成 `kind="raw"`(`file_path`/`file_name` 换成 RAW 的),而不是插入一条新记录——否则会产生两条指向"同一张照片"的记录,而且原来那条 JPEG 记录打过的标签/应用过的 `recipe` 会因为找不到对应的磁盘文件被 `prune` 误删,丢失用户已经做过的标注。

反过来的场景(先有 RAW、后来同名 JPEG 才出现)不需要处理——`scan_media` 从第一次扫描起就会忽略这份 JPEG,不会有任何代码路径尝试为它创建记录,自然也不存在"升级"这一说。

实现上,`rescan_project` 内部的"已存在检查"在按 `file_path` 精确匹配落空、且这条 `ScannedImage` 是 `kind="raw"` 时,再按"文件名主干是否匹配一条现有的 `kind='jpeg'` 记录"查一次——命中则 UPDATE(顺带触发预览缓存生成,这条记录之前是 JPEG,没有缓存),不命中则走正常插入(同样触发预览缓存生成)。

### `core::project::ImageInfo` 增补

```cpp
struct ImageInfo {
  ImageId id;
  ProjectId project_id;
  std::string file_path;
  std::string file_name;
  std::int64_t file_size;
  std::string kind;                              // 新增，"jpeg" | "raw"
  std::optional<std::string> preview_cache_path;  // 新增，kind="raw" 且缓存已生成时有值
};
```

`get_image` 的 SQL 补上 `kind, preview_cache_path` 两列。

### `core::browse::ImageRef` 增补

```cpp
struct ImageRef {
  ImageId id;
  std::string file_path;
  std::string file_name;
  std::string kind = "jpeg";                          // 新增，尾部追加，默认值保证现有聚合初始化调用点不受影响
  std::optional<std::string> preview_cache_path;       // 新增
};
```

`list_images`/`filter_by_tag` 两处 SQL 和 `ImageRef{...}` 构造点补上这两个字段(已用 `grep ImageRef{` 确认全仓库只有这两处生产构造点 + `prefetch_test.cpp` 一处测试构造点,新增尾部字段是安全的加法变更,不破坏现有调用)。`export_tag` 靠 `kind` 做路由判断(不需要 `preview_cache_path`，那个只在预览路径用得上，见 increment 4)，不需要额外查库。

### `core::export::export_tag` 路由改造(下一个 increment 才动手，这里先定设计)

现有的"有 recipe 就解码渲染编码,没有就复制/软链"二分支,扩成 `kind` × `has_recipe` 的四种组合(比配对方案的六种简化了):

| `img.kind` | 有无 `recipe_id` | 源 | 处理 |
|---|---|---|---|
| jpeg | 无 | `file_path` | 复制/软链(不变) |
| jpeg | 有 | `file_path` | `decode_jpeg_file`→`render`→`encode_jpeg_file`(不变) |
| raw | 无 | `file_path` | `raw::decode_full`→不调 `render`→`encode_jpeg_file` |
| raw | 有 | `file_path` | `raw::decode_full`→`render`→`encode_jpeg_file` |

两个容易漏的实现细节:

1. **目标文件扩展名**:两条 raw 路径输出都是 JPEG,目标文件名要把原扩展名(`.dng`/`.raf`)换成 `.jpg`(`fs::path(file_name).replace_extension(".jpg")`),不能直接复用 `ordered_name`/`resolve_collision` 现有的"原样拼文件名"逻辑。
2. **进度日志**:每次真正调用 `raw::decode_full` 前后,往 stderr 打一行"正在处理第 X/N 张",对应 spike 结论第 4 条——注意这是导出时的全量解码进度,跟 increment 3 的预览缓存生成进度是两套独立的进度提示,场景不同(一个在 `new`/`rescan`,一个在 `export`)，不合并成一套机制。

`--link` 对两条 raw 路径继续沿用 M1 已确立的限制(烘焙输出没有原始字节可软链,统一落地成真实文件)。

**依赖注入(供单元测试用)**:RAF 是私有格式,没法像 JPEG 那样用 CGImageDestination 现场合成合法测试素材,`core::raw::decode_full`/`decode_preview`/`extract_embedded_jpeg_bytes` 本身的正确性依赖 Phase 0 spike(已完成)+ 真机验收覆盖,不进 `ctest`(呼应项目里"cbreak 按键循环没法自动化测试"这个已经被接受的同类局限)。但 `export_tag` 的路由分支逻辑(四种组合各自该用哪个源、该不该调 `render`、目标扩展名对不对)是可以单测的——给 `export_tag` 新增一个可选参数:

```cpp
using RawDecodeFn = std::function<Result<decode::DecodedImage, raw::RawError>(const std::string&)>;

Result<ExportResult, ExportTagError> export_tag(db::Database& db, TagId tag_id,
                                                 const std::string& output_folder,
                                                 LinkMode link_mode = LinkMode::Copy,
                                                 RawDecodeFn raw_decode_fn = raw::decode_full);
```

签名类比 `browse::PrefetchCache` 现有的 `DecodeFn` 注入模式。默认值指向真实的 `raw::decode_full`,`cli/commands/commands.cpp` 现有调用点不用改;测试注入一个内存里现场构造 `DecodedImage` 的假函数,验证六种分支各自的源路径选择、是否调用了 `render`、目标扩展名是否正确,不需要真的链接调用 LibRaw。这是这次唯一需要改变现有函数签名的地方。

`RawError` 归入 `SkipReason` 复用现有的跳过机制(新增 `SkipReason::RawDecodeFailed`,`cli/i18n::export_skip_reason` 补一个 case),不新增独立的错误处理路径。

## 模块划分与并发模型

`core/` 新增一个子模块:

- `core/raw/`:LibRaw 封装,纯 C++ 函数式接口(不是有状态类),风格上跟 `core/decode/` 对齐("路径/字节进,像素出",同步调用)。不碰数据库,不知道 `recipe`/`kind`/配对是什么——这些是 `core::project`/`core::export` 的职责。

`core/decode`/`core/raw` 互不依赖,`core/api.cpp` 承担两者之间"按扩展名分发"这一层胶水逻辑,是这次唯一的跨模块编排代码,不下沉到任一具体模块里。

并发上不引入任何新原语:预览路径复用 `PrefetchCache` 现有的 `jthread` 后台调度(只是换了传入的解码函数);导出路径的 `raw::decode_full` 内部依赖 LibRaw 自带的 OpenMP 并行,不再叠加 `core` 自己的线程池;多张图片之间要不要并行处理是未来可选优化项,这次没有实测数据支撑这个复杂度。

## 技术选型

新增依赖:LibRaw 0.22.1(通过 Homebrew,`brew install libraw`),CMake 侧通过 `pkg-config` 接入(`find_package(PkgConfig REQUIRED)` + `pkg_check_modules(LibRaw REQUIRED IMPORTED_TARGET libraw)`)。LibRaw 传递依赖 `jpeg-turbo`/`little-cms2`/`libomp`,均由 Homebrew 自动装好,`core` 只需要链接 `PkgConfig::LibRaw`。

`libomp` 是 keg-only,`pkg-config --cflags libraw` 不会自动带出它的 include 路径(spike 阶段已经踩过这个坑,手动补了 `-I$(brew --prefix libomp)/include`),CMake 侧接入时需要显式确认链接是否需要类似的手动补丁,还是 `pkg_check_modules` 生成的 `INTERFACE_INCLUDE_DIRECTORIES` 已经够用——第一个 increment(环境+CMake 接线)里第一件事就是验证这个,不是假设能直接工作。

## 任务分解(Task Breakdown)

延续 M0/M1 的节奏:每个 increment 结束都要能实际验证,不是最后一起验收。increment 3/4 是方向调整前的旧设计,已经实现过一版(配对模型),这次按新设计重做——旧版的部分产出(`core/raw` 的两个函数、`ensure_column` 迁移机制的使用方式)照样复用,不推倒重来。

1. **环境 + CMake 接线**(已完成):`core/raw/raw.{h,cpp}` 空壳能编译链接进 `core` 静态库。
2. **`core/raw` 两个函数实现**(已完成):`extract_embedded_jpeg_bytes`/`decode_full`,真实文件验证过字节布局转换正确、颜色不跑偏。
3. **(当前 increment,取代旧的 increment 3+4)Schema + 扫描简化 + RAW 预览缓存生成**:
   - `ensure_column` 加 `kind`(两态)、`preview_cache_path`
   - `scan_media` 简化成"RAW 存在就忽略同名 JPEG",不再做配对分组
   - `core::raw::decode_preview`(half_size 解码)
   - `core/db` 补 `raw_preview_cache_dir(ProjectId)`
   - `create_project`/`rescan_project` 接入缓存生成 + `ScanProgressFn` 进度回调
   - `rescan_project` 的单方向升级逻辑(JPEG 记录后补 RAW 时原地升级)
   - 缓存文件生命周期:`rescan` 的 prune 分支、`delete_project` 都要清理对应缓存
   - 单元测试覆盖:扫描时"只有 JPEG"/"只有 RAW"/"RAW+同名JPEG(JPEG 被忽略)"/"同主干两个 RAW 不消歧"几种形状;rescan 升级路径;缓存生命周期(prune 删缓存文件、delete_project 删整个项目子目录)
   - 真机验证:真实文件跑一遍 `new`/`rescan`,确认缓存文件生成、进度提示出现、`sqlite3` 直接查表确认 `preview_cache_path` 落地正确
   - **这一步完成后暂停,不接着做 increment 4,先详细 review/验证/清理现场**
4. **预览路径接入**(已完成):`decode_jpeg_bytes` 拆分、`decode_preview_file` 门面函数(按扩展名分发,不感知 kind/缓存——路径选择这一步的职责在 `browse::PrefetchCache::set_current` 里,不在这个函数里)、`PrefetchCache` 构造点换函数、信息栏新增"来源:"展示(RAW/JPEG)。真机验证发现的真实 bug:改动前 `PrefetchCache` 一直在对 kind="raw" 的图片直接调用 `decode_jpeg_file(原始 .dng/.raf 路径)`——macOS ImageIO 自带的系统级 RAW 解码器能"正常跑"（不报错），但速度慢（真机实测到 10 秒量级）且对富士 X-Trans 支持明显更差（画面明显偏黑），这解释了两个真机报告的现象，根源是同一个：预览从来没有真正用上生成好的缓存文件。修复后（改成按 kind 选路径喂给 `decode_fn`）实测：Debug+ASan 构建下解码耗时被 ASan 插桩放大到 570-970ms（跟项目里已知的 ASan 开销倍数一致），RelWithDebInfo 构建下是 52-72ms，回到跟普通 JPEG 同一个量级。
5. **导出路由四态改造**:`export_tag` 签名新增 `RawDecodeFn` 参数、扩展名替换逻辑、进度日志、`SkipReason::RawDecodeFailed`,单元测试用注入的假解码函数覆盖四种分支。
6. **集成与真机验收**:用 `~/Pictures/raw_test_files/` 建一个真实项目过一遍 `docs/M2_PRD.md` 验收标准清单,验证同名 JPEG 确实被忽略、缓存目录里确实找不到用户照片文件夹的任何痕迹之外的文件、导出效果符合预期。

## 风险与待确认问题

延续自 `docs/M2_PRD.md`,这次工程设计阶段能定的都定了(白平衡基线、half_size 预览缓存方案、输出位深、两套独立的进度提示需求),以下几条仍然留到实现或真机验证阶段:

* **`decode_full`/`decode_preview` 的 RGB→RGBA 字节布局转换正确性**:`decode_full` 已经在 increment 2 验证过(肉眼核对真实文件颜色正常);`decode_preview` 复用同一段转换代码,理论上没有新风险,但 increment 3 里第一次真正调用它时仍然要肉眼核对一次,不能假设"跟 decode_full 共享代码所以肯定没问题"
* **色彩管理(ICC)边界**:LibRaw 输出的 8-bit sRGB 不经过 CoreGraphics ColorSync 自动匹配,`output_color=1` 已经显式指定 sRGB 输出,理论上足够,但要在真机验收时留意色彩是否有肉眼可见的偏差
* **内嵌预览缺失的边缘情况**:两台测试机身都确认是 JPEG 类型,但不代表所有 DNG/RAF 变体都是——这条路径现在只是"预览缓存缺失时的兜底",出现问题时影响面比它原来是主路径时小很多,依然不做位图兜底,留到真正遇到实例再处理
* **多张 RAW 图片的缓存生成要不要并行**:PRD 已经把"几百张真实项目的总耗时量级"列为风险,这份文档暂定沿用"每张图片各自处理,不引入额外线程池"这个简单模型(跟导出路径一致的理由:LibRaw 内部已经用 OpenMP);如果真机验证发现几百张 RAW 的 `new`/`rescan` 耗时明显不可接受,再考虑要不要在 `core::project` 这一层加一层 `jthread` 并行(每个线程各自开一个 `LibRaw` 实例处理不同文件,理论上可行,但目前没有实测数据支撑这个复杂度)
