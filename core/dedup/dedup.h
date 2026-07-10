#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/decode/decode.h"
#include "core/project/project.h"
#include "core/result.h"

// 近似重复检测——纯本地算法，不涉及云端 AI，见 docs/M3_Dedup_PRD.md/
// M3_Dedup_Eng_Design.md。这一层只管"给一批图片找重复组"，不碰标签/数据
// 库写入（core::tagging::ensure_duplicate_tag + core::find_and_tag_duplicates
// 门面负责落库，见 docs/M3_Dedup_Eng_Design.md"core/api 接口设计"一节）。
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
// file_path 是相对路径，解码预览图需要拼出绝对路径，见
// core::ai::EvaluationWorker 里同样做法的 resolve_path。
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
//    况，仍然应该归成一组。单张图片解码失败时跳过(不参与任何合并，日
//    志打到 stderr)，不让一张坏图拖垮整簇的比对。
// 4. 并查集结果里，成员数 >= 2 的集合才算一个"重复组"输出；成员数
//    1(没有跟任何其它图片凑到一起)的不输出。
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
// on_progress 在每处理完一个候选簇(不论是否成簇)时回调一次，done 是已
// 处理的候选簇数、total 是候选簇总数。
std::vector<DuplicateGroup> find_duplicates(db::Database& db, const std::string& root_path,
                                             const std::vector<project::ImageId>& image_ids,
                                             int time_window_seconds = 10, int hash_threshold = 5,
                                             DedupProgressFn on_progress = nullptr);

struct DedupSummary {
  int group_count;
  int tagged_count;             // 被打上 duplicate 标签的图片总数(不含每组里被保留的那一张)
  int unevaluated_image_count;  // 这次范围内还没跑过选片辅助评估的图片数(不分是否在重复组里)
};

// 编排层——跟 find_duplicates 不同，这个函数会碰数据库/标签：统计未评
// 估数 -> 清空 image_ids 范围内的旧 duplicate 标记 -> find_duplicates
// 分组 -> 给每组除 keep_id 外的成员打标签。放在同一个 core/dedup 模块
// 里，不是违反"纯算法层"的说法——跟 core/export 的 export_tag(db::
// Database&, ...) 同一个先例：模块以功能命名(dedup/export)，内部按需
// 组合其它模块(tagging/project)完成一次完整的用户可见操作，"纯算法层"
// 说的是 find_duplicates 这一个函数，不是整个模块。project_id 只用来
// 定位 duplicate 标签所在的项目(标签按项目隔离)和取 root_path，不代表
// 扫描范围——扫描范围是 image_ids，由调用方自己解析好(整个项目还是某
// 个标签的子集)再传进来。core/api.h 的同名门面函数只是开默认库、转调
// 这个函数的一层薄封装，方便单元测试指向临时测试库。
Result<DedupSummary, project::ProjectNotFoundError> find_and_tag_duplicates(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    DedupProgressFn on_progress = nullptr);

// 仅供单元测试使用——decode_fn 可注入，不需要真的解码 JPEG 文件就能验证
// 时间聚类、hamming 距离分组、keep_id 选择这些逻辑，规避真实 JPEG 有损
// 压缩给像素级精确控制带来的不确定性。跟 core/ai/evaluation.h 的
// detail::request_evaluation_impl(注入 HttpPostFn)是同一个模式。上面的
// find_duplicates 是这个函数在默认 decode_fn 参数下的一层薄封装。
namespace detail {

using PreviewDecodeFn =
    std::function<Result<decode::DecodedImage, decode::DecodeError>(const std::string&)>;

std::vector<DuplicateGroup> find_duplicates_impl(db::Database& db, const std::string& root_path,
                                                  const std::vector<project::ImageId>& image_ids,
                                                  int time_window_seconds, int hash_threshold,
                                                  DedupProgressFn on_progress,
                                                  PreviewDecodeFn decode_fn);

}  // namespace detail

}  // namespace pzt::core::dedup
