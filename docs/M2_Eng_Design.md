# PicZTream (PZT) Milestone 2 工程设计文档

## 背景

`docs/M2_PRD.md` 已经拍板了 M2 的产品决策(纯 DNG/RAF、按需触发 LibRaw 解码、8-bit sRGB 输出、RAW+JPEG 同名配对模型),按 `AGENTS.md` 的工程契约,具体的表结构、模块划分、接口签名要落到这份文档,implementation 应在本文档评审通过后再开始。

## LibRaw 解码性能验证结论

对应 M2_PRD.md"技术方案概要"提到的性能未知项,已经用一个独立探针(`spikes/libraw_probe/`)对用户本地真实拍摄的素材验证过,完整数据见该目录下的 `results.md`,这里摘要对设计有直接影响的结论:

1. **内嵌预览提取(`unpack_thumb` + `dcraw_make_mem_thumb`)远低于 100ms 延迟预算**:两台真实机身(徕卡 Q3、富士 X-T5)都在 3ms 以内完成,culling 路径"不触发全量解码"的核心假设成立。
2. **两台机身的内嵌预览格式都是 `LIBRAW_IMAGE_JPEG`**,不是位图,`extract_embedded_jpeg_bytes` 不需要实现位图兜底分支。
3. **富士 X-Trans 全量解码比徕卡拜耳阵列慢 2.4-2.6 倍**(换算每兆像素耗时,X-Trans 约是拜耳的 4 倍),符合"X-Trans 非规则阵列插值窗口更大"的预期,这次有了具体数字:徕卡约 2.0-2.2 秒/张(60MP),富士约 5.3 秒/张(40MP)。
4. **全量解码是秒级耗时,`pzt export` 需要为需要真正解码的 RAW 图片加进度提示**,这是 spike 之前 PRD 风险清单里悬而未决的问题,现在有数据支撑要不要做——静默跑几十张、每张几秒,总耗时到分钟级,不给任何反馈体验会很差。
5. **不需要为全量解码额外引入并发设计**:LibRaw 内部已经用 OpenMP 并行,`pzt export` 沿用 M1"每张图片各自处理,不需要常驻后台队列"的模型即可。

### 对 core 设计的直接影响

- 预览路径:`core::raw::extract_embedded_jpeg_bytes` 只处理 JPEG 类型,不做位图分支(真机测试没遇到过再加)
- 处理路径:`core::raw::decode_full` 不需要 `jthread`/线程池包装,直接调用 LibRaw 单线程 API(LibRaw 内部自己用 OpenMP)
- `pzt export` 遇到需要走 `raw::decode_full` 的图片时,往 stderr 打一行"正在处理 X/N"的轻量进度日志(沿用现有延迟日志的输出通道和风格,不引入进度条之类的复杂 UI)

## 数据库 Schema 设计

`images` 表新增两列(复用 M1 已经建立的 `ensure_column` 幂等迁移机制):

```sql
ALTER TABLE images ADD COLUMN kind TEXT NOT NULL DEFAULT 'jpeg';
-- 'jpeg' | 'raw' | 'raw_jpeg'。M0/M1 时代的旧库迁移时所有已有行都落在默认值
-- 'jpeg'，行为完全不变——这也是选 TEXT 默认值而不是要求显式回填的原因。

ALTER TABLE images ADD COLUMN raw_path TEXT;
-- kind != 'jpeg' 时才有值：kind='raw' 时等于 file_path 本身（冗余存储换
-- 一个不用为 kind='raw' 单独 if 分支的下游查询）；kind='raw_jpeg' 时是配对
-- 的 RAW 文件相对路径。
```

`file_path`/`file_name` 两列的既有语义不变,延续代表"culling 预览用哪个文件":纯 JPEG → JPEG 本身(不变);纯 RAW → RAW 本身;RAW+JPEG 配对 → JPEG 伴侣文件(不是 RAW)。这个设计的好处是 `core::decode_preview_file` 只需要看扩展名分发,不需要知道 `kind`;`export_tag` 的路由只需要查 `kind` + `raw_path`,不需要额外 JOIN。

不新增唯一索引约束 `raw_path`——配对关系的正确性由扫描阶段的分组逻辑(见下)在 C++ 层保证,不需要 DB 层再加一层约束,避免过度设计。

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

}  // namespace pzt::core::raw
```

`decode_full` 的实现要点:`libraw_processed_image_t` 返回的是 3 字节/像素紧凑排列(`colors=3, bits=8`),转换成 `DecodedImage` 的 4 字节/像素(RGBA,alpha 位跳过)布局需要一次逐像素展开拷贝,不是简单的 `memcpy`。这一步以及最终的字节序/通道顺序要不要跟现有 `decode_jpeg_file` 严格对齐,是实现阶段第一个要写单元测试(用一个已知 RGB 值的小图或者直接跟真机验收的肉眼对比)锁定的地方,避免颜色通道错位这种"能跑但是错的"问题(呼应 PRD 风险清单里"色彩管理边界变化"那条)。

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

### `core::project` 增补:格式识别与配对

```cpp
// project.cpp 内部，is_jpeg 旁边新增:
bool is_raw(const fs::path& p);  // 扩展名在 {.dng, .raf} 内，大小写不敏感

// ScannedImage 增加两个字段：
struct ScannedImage {
  std::string relative_path;
  std::string file_name;
  std::int64_t file_size;
  std::string kind;                        // "jpeg" | "raw" | "raw_jpeg"
  std::optional<std::string> raw_relative_path;  // kind != "jpeg" 时有值
};
```

`scan_jpegs` 改名 `scan_media`,先各自扫出 JPEG 类和 RAW 类两个文件列表,按"所在目录 + 文件名主干(不含扩展名,大小写敏感比较主干本身)"分组:

- 一组里恰好一个 JPEG + 一个 RAW → 合并成一条 `kind="raw_jpeg"`,`relative_path`/`file_name`/`file_size` 取 JPEG 那份(对齐"预览用 JPEG"的设计),`raw_relative_path` 记 RAW 那份的路径
- 只有 JPEG → `kind="jpeg"`(M0/M1 现有行为,不变)
- 只有 RAW → `kind="raw"`,`raw_relative_path` 等于 `relative_path` 自己
- 其它形状(比如同一主干出现两个 RAW 文件)不做配对,每个文件各自成一条独立记录——这类输入 PRD 已经说明"理论上不该发生",不强行发明消歧规则

`is_raw` 用一个 `constexpr` 字符串数组表达受支持的扩展名集合,不写死在多处判断里,给"未来加 CR2/CR3/NEF/ARW"这个已知的未来考虑留好扩展点(不是现在就做插件化设计,只是不把判断逻辑分散复制)。

### `core::project::rescan_project` 的配对升级逻辑

现有 `rescan_project` 只有"路径已存在就跳过、不存在就插入"这个二分支。M2 需要处理第三种情况:新扫到的文件与一条已有记录构成配对升级——比如项目建的时候文件夹里只有 JPEG,之后才把对应的 RAW 拷贝进来。这种情况下,已有的 `kind="jpeg"` 记录要被 **UPDATE** 成 `kind="raw_jpeg"` 并补上 `raw_path`,而不是插入一条新记录(否则会出现两条指向同一张照片的记录,违反"配对文件不重复计数"这条验收标准)。

实现上,`rescan_project` 内部的"已存在检查"要从"按 `file_path` 查一行是否存在"扩展成"按配对规则查:这个新扫到的文件的主干,是否匹配一条已有记录(不管是匹配它的 `file_path` 主干还是 `raw_path` 主干)",匹配到则 UPDATE 而不是跳过或插入。这是任务分解里单独的一个 increment 和一组单元测试,逻辑形状跟"新增/删除"这两个现有分支不一样,值得独立验证。

### `core::project::ImageInfo` 增补

```cpp
struct ImageInfo {
  ImageId id;
  ProjectId project_id;
  std::string file_path;
  std::string file_name;
  std::int64_t file_size;
  std::string kind;                         // 新增，供信息栏展示来源类型
  std::optional<std::string> raw_path;       // 新增
};
```

`get_image` 的 SQL 补上 `kind, raw_path` 两列。

### `core::browse::ImageRef` 增补

```cpp
struct ImageRef {
  ImageId id;
  std::string file_path;
  std::string file_name;
  std::string kind = "jpeg";                 // 新增，尾部追加，默认值保证现有聚合初始化调用点不受影响
  std::optional<std::string> raw_path;        // 新增
};
```

`list_images`/`filter_by_tag` 两处 SQL 和 `ImageRef{...}` 构造点补上这两个字段(已用 `grep ImageRef{` 确认全仓库只有这两处生产构造点 + `prefetch_test.cpp` 一处测试构造点,新增尾部字段是安全的加法变更,不破坏现有调用)。`export_tag` 靠这两个新字段做路由判断,不需要额外查库。

### `core::export::export_tag` 路由改造

现有的"有 recipe 就解码渲染编码,没有就复制/软链"二分支,扩成 `kind` × `has_recipe` 的六种组合:

| `img.kind` | 有无 `recipe_id` | 源 | 处理 |
|---|---|---|---|
| jpeg | 无 | `file_path` | 复制/软链(不变) |
| jpeg | 有 | `file_path` | `decode_jpeg_file`→`render`→`encode_jpeg_file`(不变) |
| raw | 无 | `raw_path`(=`file_path`) | `raw::decode_full`→不调 `render`→`encode_jpeg_file` |
| raw | 有 | `raw_path` | `raw::decode_full`→`render`→`encode_jpeg_file` |
| raw_jpeg | 无 | `file_path`(JPEG 伴侣) | 复制/软链,不碰 RAW,不解码 |
| raw_jpeg | 有 | `raw_path` | `raw::decode_full`→`render`→`encode_jpeg_file` |

两个容易漏的实现细节:

1. **目标文件扩展名**:纯 RAW 图片(不管有没有 recipe)和"配对+有 recipe"这三条路径,输出都是 JPEG,目标文件名要把原扩展名(`.dng`/`.raf`)换成 `.jpg`(`fs::path(file_name).replace_extension(".jpg")`),不能直接复用 `ordered_name`/`resolve_collision` 现有的"原样拼文件名"逻辑,否则会生成一个内容是 JPEG、后缀却是 `.dng` 的文件。
2. **进度日志**:每次真正调用 `raw::decode_full` 前后,往 stderr 打一行"正在处理第 X/N 张"(X = 当前已处理数,N = 这次导出总数,不需要区分是不是走 RAW 路径的子计数,直接复用外层遍历的 `index`/`images.size()`,保持实现简单),对应 spike 结论第 4 条。

`--link` 对纯 RAW 和配对+recipe 这几条新路径继续沿用 M1 已确立的限制(烘焙输出没有原始字节可软链,统一落地成真实文件)。

**依赖注入(供单元测试用)**:RAF 是私有格式,没法像 JPEG 那样用 CGImageDestination 现场合成合法测试素材,`core::raw::decode_full`/`extract_embedded_jpeg_bytes` 本身的正确性依赖 Phase 0 spike(已完成)+ 真机验收覆盖,不进 `ctest`(呼应项目里"cbreak 按键循环没法自动化测试"这个已经被接受的同类局限)。但 `export_tag` 的路由分支逻辑(六种组合各自该用哪个源、该不该调 `render`、目标扩展名对不对)是可以单测的——给 `export_tag` 新增一个可选参数:

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

延续 M0/M1 的节奏:每个 increment 结束都要能实际验证,不是最后一起验收。

1. **环境 + CMake 接线**:`brew install libraw pkg-config`(spike 阶段已装),`core/CMakeLists.txt` 接入 `pkg_check_modules`,新建空壳 `core/raw/raw.{h,cpp}` 能编译链接进 `core` 静态库,不需要真功能。第一件事验证 `libomp` include 路径问题在 CMake 环境下是否需要手动处理。
2. **`core/raw` 两个函数实现**:`extract_embedded_jpeg_bytes`/`decode_full`,用 `~/Pictures/raw_test_files/` 的真实文件手动验证(临时调试命令,或者直接复用/扩展 spike probe 的代码路径做验证,不需要等 core/project 的 schema 改动就绪)。这一步要锁定 `libraw_processed_image_t` 到 `DecodedImage` 的字节布局转换正确性(肉眼对比真机渲染效果,颜色不能跑偏)。
3. **Schema 迁移 + `scan_media` 配对分组**(`create_project` 路径):`ensure_column` 加两列,`is_raw`/`scan_media`/分组逻辑,单元测试覆盖"只有 JPEG"、"只有 RAW"、"配对"、"同主干两个 RAW 不配对"这几种分组形状。
4. **`rescan_project` 的配对升级逻辑**:单独的单元测试覆盖"先有 JPEG 后补 RAW"、"先有 RAW 后补 JPEG"两种升级路径,确认不产生重复记录。
5. **预览路径接入**:`decode_jpeg_bytes` 拆分、`decode_preview_file` 门面函数、`PrefetchCache` 构造点换函数、信息栏新增"来源:"展示(RAW/JPEG/RAW&JPEG)。真机验收:用真实 RAW 项目过一遍 `pzt open`,确认切图延迟无感知差异。
6. **导出路由六态改造**:`export_tag` 签名新增 `RawDecodeFn` 参数、扩展名替换逻辑、进度日志、`SkipReason::RawDecodeFailed`,单元测试用注入的假解码函数覆盖六种分支。
7. **集成与真机验收**:用 `~/Pictures/raw_test_files/` 建一个真实项目过一遍 `docs/M2_PRD.md` 验收标准清单,需要手动构造至少一组同名 RAW+JPEG(从某个 RAF/DNG 提取内嵌预览另存为同名 JPEG,模拟配对场景),实测一次真实的"应用风格的 RAW 图片导出"耗时,确认进度日志按预期出现。

## 风险与待确认问题

延续自 `docs/M2_PRD.md`,这次工程设计阶段能定的都定了(白平衡基线、内嵌预览提取方式、输出位深、进度提示需求),以下几条仍然留到实现或真机验证阶段:

* **`decode_full` 的 RGB→RGBA 字节布局转换正确性**:目前只有理论对齐(`kCGImageAlphaNoneSkipLast` + `kCGBitmapByteOrder32Big`),没有实际写代码验证过,increment 2 需要专门花时间肉眼核对颜色是否正确,不能想当然
* **色彩管理(ICC)边界**:LibRaw 输出的 8-bit sRGB 不经过 CoreGraphics ColorSync 自动匹配(这条路径完全绕开了 ImageIO),M0/M1"信任系统默认处理"的假设在这里不成立,`output_color=1` 已经显式指定 sRGB 输出,理论上足够,但要在真机验收时留意色彩是否有肉眼可见的偏差
* **内嵌预览缺失的边缘情况**:两台测试机身都确认是 JPEG 类型,但不代表所有 DNG/RAF 变体都是——真机测试之外的机型如果出现例外,`extract_embedded_jpeg_bytes` 目前会归为 `DecodeFailed`,不做位图兜底,留到真正遇到实例再处理
* **RAW+JPEG 配对判定规则边界**:同名不同扩展名是最简单的规则,"同一主干两个 RAW"这类不该发生的输入,选择"不配对、各自独立"而不是报错拒绝整个扫描——这个宽松处理策略在真机测试中如果发现体验不好(比如用户以为配对了实际上没有),可以后续调整
