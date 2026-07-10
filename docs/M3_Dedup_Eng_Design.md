# PicZTream (PZT) Milestone 3 工程设计文档（近似重复检测）

## 背景

`docs/M3_Dedup_PRD.md` 定的产品行为：按"拍摄时间聚类 + 感知哈希比较"两步找出近似重复的照片组，每组自动选一张保留、其余打上系统标签。这是纯本地算法，不涉及云端 AI，架构上跟选片辅助评估（`core/ai/`）完全独立。

唯一入口是 `pzt open` 控制台命令 `/dedup *`/`/dedup <标签名>`（阻塞式，不做独立的顶层 CLI 命令）。即便只有一个调用方，这个命令本身就有两种范围模式——`*`（整个项目）和 `<标签名>`（子集）——所以 `core::dedup`/`core::find_and_tag_duplicates` 这一层仍然不能只认"项目"这个范围概念，要能接受一份**调用方已经解析好的图片列表**：`*` 和 `<标签名>` 只是产生这份列表的两种不同方式，算法层不需要、也不应该知道范围是怎么来的。

## 现有代码基础

* **`docs/M3_Eng_Design.md` 的 `handle_ai_console_command`/`handle_ai_eval_command`**：`/dedup` 分支照抄 `/ai_eval` 分支的形状——`:` 输入以 `/` 开头时解析出命令名和剩余参数，`*` 或 `<标签名>` 两种范围解析方式（`list_images`/`find_tag_by_name`+`filter_by_tag`）完全复用，不重新实现一遍范围解析逻辑。CLI 层的命令分发本身不写单元测试（跟现有命令处理函数一致），只测它调用的 `core::` 函数。
* **`cli/menu/tag_menu.cpp` 里已有的 `prompt_and_read_key`**：`/dedup` 触发时"N 张照片还没评估，是否继续？(y/N)"这个单键确认直接复用这个原语，不新增输入机制。
* **`core/tagging/tagging.h` 的 `ensure_reject_tag`**：幂等地"查不到就创建"一个系统标签（`is_system=1`），"废片"标签是先例。这次的 duplicate 标签照抄同样的模式，新增一个平行的 `ensure_duplicate_tag`。
* **`core/decode/decode.h` 的 `resize_rgba`**：感知哈希需要的降采样可以复用这个，不用重新写一遍缩放逻辑；灰度转换（RGB→亮度）目前没有现成的工具函数，这次在 `core/dedup/` 内部写一个，不上升成 `core/decode` 的通用能力（只有这一个消费者，不提前抽象）。
* **`core::decode_preview_file`**：解码这一步复用跟浏览、AI 评估同一条路径（core/api.h 的门面函数），不为了算哈希单独实现一套解码。
* **`core/project/project.h` 的 `ImageInfo`/`ai::EvaluationInfo`**：选保留哪一张要用到 `overall_score()`，这个函数已经在 `core/ai/evaluation.h` 里，直接复用，不重新实现一遍平均值计算。

不需要新的数据库表——"重复组"本身不持久化（一组照片除了都被打上 duplicate 标签之外，组内成员关系不单独存一份），"保留哪张"的判断只在跑 `pzt dedup` 这一次运行期间用到，跑完就不需要再查。这是一个刻意的简化，见"风险与待确认问题"。

## core/api 接口设计

### `core/dedup/dedup.h`/`.cpp`：纯算法层，不碰数据库/标签

```cpp
namespace pzt::core::dedup {

using ImageHash = std::uint64_t;

// 灰度降采样(9x8) + 相邻像素梯度比较，产出一个 64-bit 差异哈希(dHash)。
// 9x8 降采样是 dHash 的标准做法——每行 9 个像素，跟右边相邻的比较产出 8
// 个 bit，8 行凑够 64 bit，不是随便选的数字。选 dHash 不选 pHash：dHash
// 不需要离散余弦变换，实现和理解成本都更低，对近似重复(同一场景近乎相
// 同的照片，不是"风格不同但语义相似"这种更弱的相似)这个场景够用。
ImageHash compute_dhash(const decode::DecodedImage& image);

// 汉明距离(两个哈希按位异或后数 1 的个数)，越小越相似，取值范围 0-64。
int hamming_distance(ImageHash a, ImageHash b);

struct DuplicateGroup {
  std::vector<project::ImageId> image_ids;  // 组内所有成员，包含被选中保留的那一张
  project::ImageId keep_id;
};

using DedupProgressFn = std::function<void(int done, int total)>;

// 在给定的一批图片里找重复组(只包含真正找到重复的组，落单的图片不会出
// 现在返回值里)。image_ids 是调用方已经解析好的范围——"整个项目"还是
// "带某个标签"由调用方决定(core::list_images/core::filter_by_tag)，这
// 个函数不知道、也不需要知道范围是怎么来的。root_path 是这批图片所属项
// 目的根目录(project::ProjectSummary::root_path)——images 表的
// file_path 是相对路径，要解码出像素才能算哈希，必须先拼出绝对路径，跟
// core::ai::EvaluationWorker 里 resolve_path 是同一个逻辑，各自维护一份
// (就几行，不共享)。
//
// 算法：
// 1. 查询这批图片的(image_id, captured_at, file_path, kind,
//    preview_cache_path)，按 captured_at 排序；captured_at 为 NULL 的图
//    片直接跳过，不参与任何分组(没有时间信息就没法判断"是不是紧挨着拍
//    的"，硬要全量比对代价太高)。
// 2. 滑动窗口分候选簇：相邻两张图片的 captured_at 差值超过
//    time_window_seconds 就切一个新簇——候选簇是"presumably 同一次连
//    拍/包围曝光"的粗筛，不是最终分组结果。
// 3. 簇内两两算 dHash 汉明距离，距离 <= hash_threshold 的两张用并查集
//    (union-find)合并——用并查集而不是简单的"每张只跟第一张比"，是为
//    了处理 A 像 B、B 像 C、但 A 跟 C 的距离刚好超过阈值这种传递性情
//    况，仍然应该归成一组。
//    单张图片解码失败时跳过(不参与任何合并，日志打到 stderr)，不让一
//    张坏图拖垮整簇的比对——这一点原设计没写，实现阶段补上：真实照片解
//    码失败不该是个未定义行为。
// 4. 并查集结果里，成员数 >= 2 的集合才算一个"重复组"输出；成员数
//    1(没有跟任何其它图片凑到一起，或者解码失败被跳过后只剩自己)的不
//    输出。
// 5. 组内选 keep_id：只有这一组内**所有**成员都跑过选片辅助评估
//    (core::project::get_image(...).evaluation 对组里每一张都有值)时，
//    才按 overall_score() 最高的选——"选质量最高的"这个判断本身依赖评
//    估结果，只要这一组里有一张没评估过就不比较分数。这一组里有任意一
//    张没评估过(不管是这一组全没评估还是部分没评估)，退化成按
//    captured_at 最新的选；分数/captured_at 也相等的极端情况兜底选
//    image_id 最小的，保证确定性。**这个规则按组独立判断**——同一次调
//    用里，有的组因为全员评估过按分数选，有的组因为缺评估退化成按时间
//    选，互不影响。
//
// on_progress 在每处理完一个候选簇(不论是否成簇)时回调一次，done/total
// 是候选簇的处理进度，用于 CLI 展示进度。
std::vector<DuplicateGroup> find_duplicates(db::Database& db, const std::string& root_path,
                                             const std::vector<project::ImageId>& image_ids,
                                             int time_window_seconds = 10, int hash_threshold = 5,
                                             DedupProgressFn on_progress = nullptr);

}  // namespace pzt::core::dedup
```

`time_window_seconds`/`hash_threshold` 的默认值（10 秒、5 bit）是经验值，没有用真实照片数据验证过——见"风险与待确认问题"。做成函数参数而不是写死的常量，方便以后需要调参或者做成用户可配置时不用改函数签名。

**实现阶段补的一处设计**（原设计没覆盖）：`find_duplicates` 的解码依赖（内部调什么函数把文件路径变成像素）是可注入的——真实实现是 `namespace detail { find_duplicates_impl(..., PreviewDecodeFn decode_fn) }`，`find_duplicates` 只是拿默认 `decode_fn`（跟 `core::decode_preview_file` 同一套 JPEG/RAW 分发逻辑，`core/dedup/dedup.cpp` 内部单独维护一份，不能反向 `#include "core/api.h"`，见文件内注释）调一层薄封装。这是跟 `core/ai/evaluation.h` 的 `detail::request_evaluation_impl(..., HttpPostFn)` 同一个模式：单元测试要验证时间聚类边界、并查集传递性合并、keep_id 选择这些逻辑，如果每个用例都要造出真实 JPEG 文件走一遍有损压缩，像素级精确控制会变得很脆弱（压缩伪影可能改变相邻像素的大小关系，进而改变 dHash 的具体 bit），不值得为了"更像端到端"而牺牲测试的确定性——注入一个假的 `decode_fn`（直接返回构造好的 `DecodedImage`）就能精确控制每张"图片"对应的 dHash，聚类/并查集/keep_id 这些纯逻辑因此可以在完全确定性的条件下验证。

### `core/tagging`：新增 `ensure_duplicate_tag`

```cpp
constexpr const char* kDuplicateTagName = "重复";  // 跟 kRejectTagName("废片")同一个惯例,中文,用户在 pzt open 里直接看到这个名字
TagId ensure_duplicate_tag(db::Database& db, ProjectId project_id);
```
实现照抄 `ensure_reject_tag`，只换标签名字。

### `core/api.h`：门面函数

```cpp
struct DedupSummary {
  int group_count;
  int tagged_count;             // 被打上 duplicate 标签的图片总数(不含每组里被保留的那一张)
  int unevaluated_image_count;  // 这次范围内还没跑过选片辅助评估的图片数(不分是否在重复组里)
};

// project_id 只用来找 duplicate 标签所在的项目(标签是按项目隔离的，见
// core/tagging)，不代表扫描范围——扫描范围是 image_ids 参数，可以是这
// 个项目的全部图片，也可以是一个子集(比如某个标签下的图片)，由调用方
// 自己解析好再传进来。project_id 理论上可以从 image_ids[0] 反查，但要
// 求调用方显式传，语义更直接，也不用处理"image_ids 为空、查不到
// project_id"这种边界情况。
Result<DedupSummary, ProjectNotFoundError> find_and_tag_duplicates(
    ProjectId project_id, const std::vector<ImageId>& image_ids,
    dedup::DedupProgressFn on_progress = nullptr);
```

内部执行顺序：

1. 遍历 `image_ids`，统计有多少张 `evaluation` 是 `nullopt`，记进 `unevaluated_image_count`（这一步只统计、不阻塞、不代为触发评估——`docs/M3_Dedup_PRD.md`"非目标"一节明确规定）。
2. **清空旧标记**：找到 `duplicate` 系统标签（`ensure_duplicate_tag`），只把 `image_ids` 这个范围内、带这个标签的图片摘掉标签——**不是清空整个项目**，范围外的图片（比如全项目扫描之后又单独对某个标签跑了一次）不受影响，见 `docs/M3_Dedup_PRD.md`"重新运行"一节。第一次运行时这批图片可能都还没有标签，这一步对没打过标签的图片是空操作，不需要特殊分支。
3. `project::open_project(db, project_id)` 拿 `root_path`，调 `dedup::find_duplicates(db, root_path, image_ids, ...)` 拿到分组结果。
4. 对每个组里除 `keep_id` 之外的每张图调 `tagging::add_tag`，累加 `tagged_count`。

第 2 步"先摘光再重新打"和第 3-4 步"重新分组打标签"都在同一次调用里完成，中间不会有"标签已经被清空、但新的还没打上"这种状态被外部看到太久——`pzt open` 这类只读查询不会撞上这个中间态（SQLite 默认隔离级别下，读操作看到的是已经提交的行，`add_tag`/`remove_tag` 各自是独立的小事务，理论上确实存在极短的窗口读到"标签被清空但还没打回来"的中间状态，可接受，见"风险与待确认问题"）。

**唯一调用方 `handle_dedup_command` 怎么用这个门面**：`*` 时自己先 `list_images(project_id)` 拿到全项目的图片列表，`<标签名>` 时用 `find_tag_by_name`+`filter_by_tag` 拿到子集，再把解析好的 `image_ids` 传给 `find_and_tag_duplicates`——"范围怎么来的"这个语义完全是 CLI 层的约定，这个门面函数本身不知道、也不需要知道。

## CLI 接线

`cli/commands/browse.cpp` 的 `/dedup` 分支，接到 `docs/M3_Eng_Design.md`"CLI 接线"一节定的 `handle_ai_console_command` 分发器上，跟 `ai_eval`/`tasks` 平级多一个分支：

```cpp
if (command == "dedup") {
  std::string scope;
  iss >> scope;
  return handle_dedup_command(project_id, scope, banner_row, start_col, content_cols);
}
```

`handle_dedup_command`：

```cpp
std::string handle_dedup_command(pzt::core::ProjectId project_id, const std::string& scope,
                                  int banner_row, int start_col, int content_cols) {
  std::vector<pzt::core::ImageId> image_ids;
  if (scope == "*") {
    for (const auto& ref : pzt::core::list_images(project_id)) image_ids.push_back(ref.id);
  } else {
    auto tag_id = pzt::core::find_tag_by_name(project_id, scope);
    if (!tag_id) return pzt::cli::i18n::msg_ai_eval_tag_not_found(scope);  // 复用同一条"标签不存在"文案
    auto filtered = pzt::core::filter_by_tag(*tag_id);
    if (!filtered.ok()) return pzt::cli::i18n::err_filter_failed();
    for (const auto& ref : filtered.value()) image_ids.push_back(ref.id);
  }

  int unevaluated = 0;
  for (auto id : image_ids) {
    auto info = pzt::core::get_image(id);
    if (!info || !info->evaluation) ++unevaluated;
  }
  if (unevaluated > 0) {
    char c = prompt_and_read_key(pzt::cli::i18n::msg_dedup_confirm_unevaluated(unevaluated),
                                  banner_row, start_col, content_cols);
    if (c != 'y' && c != 'Y') return "";  // 取消,静默
  }

  // 阻塞:接下来这一步(find_and_tag_duplicates 内部的 find_duplicates)
  // 可能跑几秒到几十秒,期间 pzt open 冻结,不接受任何输入——这是刻意的
  // 简化,见 docs/M3_Dedup_PRD.md"非目标"一节。
  auto result = pzt::core::find_and_tag_duplicates(project_id, image_ids, /*on_progress=*/nullptr);
  if (!result.ok()) return pzt::cli::i18n::err_dedup_failed();
  return pzt::cli::i18n::msg_dedup_result(result.value().group_count, result.value().tagged_count);
}
```

**这里没有传 `on_progress` 回调**——`/dedup` 是阻塞 `pzt open` 主循环的，主循环本身在这段时间内没有机会重绘（跟其它所有 `handle_*` 子流程一样，读键循环阻塞期间画面不会更新，见 `docs/M3_Eng_Design.md`"CLI 接线"一节里 `space`/`g`/`r` 这些子菜单的既有说明），传一个进度回调也没地方画，不如不传，省一次白跑的开销。真机验收阶段如果发现"用户等太久以为卡死"是个真问题，再考虑要不要在这个阻塞期间画点什么。

## 任务分解

1. **`core/dedup/dedup.h`/`.cpp`**：`compute_dhash`/`hamming_distance`/`find_duplicates`（时间聚类+并查集分组+选 keep_id）。单元测试用合成的 `DecodedImage`（构造几张几乎相同的纯色/渐变图 + 几张明显不同的，配合人工设定的 `captured_at`）验证聚类边界（同一簇内该分到一组的分到一组、不该分到一起的不分；时间窗口之外的不会被比较；A-B、B-C 相似但 A-C 距离超阈值时仍然并成一组）——不需要真实照片，纯算法可以完全用合成数据覆盖。`keep_id` 规则要单独测两种场景：组内全部评估过（按分数选）、组内至少一张没评估过（不管是全没评估还是部分评估，都退化成按 `captured_at` 最新选）
2. **`core::tagging::ensure_duplicate_tag`** + **`core::find_and_tag_duplicates`门面**：落库逻辑，单元测试验证：标签正确打到非 `keep_id` 的图片上；`unevaluated_image_count` 统计正确；**重新运行会先清空这次范围内的 duplicate 标签再重新打**（构造一个"上一轮运行遗留的标签、这一轮分组结果不一样"的场景，验证旧标签被正确清掉、不会新旧混杂；再构造一个"范围外的图片带着旧标签"的场景，验证只清范围内的、不动范围外的）
3. **控制台命令接线**：`handle_ai_console_command` 分发器新增 `dedup` 分支、`handle_dedup_command`（范围解析、未评估计数、`prompt_and_read_key` 做 y/N 确认、调门面函数、结果摘要文案）——跟其它 `handle_*` 子流程一样不做直接的自动化测试，`core::find_and_tag_duplicates` 的单元测试已经覆盖了业务逻辑，这一层只做真机验证
4. **真机验收**：找一批真实的连拍/包围曝光照片跑一遍，`/dedup *` 和 `/dedup <标签名>` 都验证一遍；人工确认分组结果合理、`keep_id` 选择符合直觉、整体运行时间可接受；确认有未评估图片时能看到 y/N 提示，按其它键能正确取消不产生标签改动；顺手验证一下明显不相关的照片不会被误判；验证重新运行时旧标记确实被清空重打，而不是新旧标记堆在一起

## 风险与待确认问题

延续自 `docs/M3_Dedup_PRD.md`（相似度阈值、时间聚类窗口怎么定，都没有真实数据验证过，这次凭经验起步）：

* **"重复组"不持久化的取舍**：这次没有单独存一张"重复组"表，组内关系跑完 `/dedup` 之后就不再记录（只留下一批共享 duplicate 标签的图片，具体谁跟谁是一组、组内谁是谁的重复品，如果不重新跑一遍算法就查不出来了）。如果以后发现"能看到具体分组"这件事对用户有价值（比如想知道某张照片具体是哪几张的重复），需要补一张 `duplicate_groups`/`image_duplicate_group` 关联表，这次先不加，避免为一个还没验证过的需求预先设计存储层
* **簇内 O(k²) 比对的性能**：候选簇如果异常大（比如一次导入了几百张连拍），簇内两两比较仍然是平方级——这次没有实测过真实连拍量级下这一步的耗时，真机验收阶段需要留意
* **`ensure_duplicate_tag` 会不会跟用户已有的同名标签冲突**：如果用户在这个功能之前就手动建过一个叫"duplicate"的标签，`ensure_duplicate_tag` 会直接复用那个标签（`find_tag_by_name` 找到就返回，不区分是不是系统创建的）——这跟 `ensure_reject_tag` 面临的是同一个问题，沿用同样的处理方式（不特殊处理，交给用户自己保证标签名不冲突）
* **"先清空再重打"中途失败的中间状态**：`find_and_tag_duplicates` 如果在摘掉旧标签之后、重新打完新标签之前中断（进程崩溃、数据库出问题），项目会短暂停留在"duplicate 标签全部被清空、新的还没打上"的状态——这不是数据丢失（重新触发一次 `/dedup` 就能恢复），但用户这时候如果看到浏览画面，会看到"所有重复标记都不见了"，可能引起困惑。这次不做事务包裹（摘标签、分组算法、打标签是三个不同粒度的操作，算法这一步可能跑几十秒，不适合整个包在一个长事务里），接受这个小概率的中间态风险，真机使用中如果发现是个真问题再考虑要不要引入更细的状态跟踪
* **单入口简化了什么**：去掉独立顶层命令之后，不再需要考虑"两个入口并发触发会不会互相干扰"（比如 `pzt dedup` 和 `:/dedup` 同时跑，各自摘标签再重打，谁的结果最终生效）——`/dedup` 阻塞 `pzt open` 主循环，同一个进程内天然不存在并发触发的问题；如果以后真的要支持不阻塞的异步版本，这一类并发问题需要重新纳入考虑
