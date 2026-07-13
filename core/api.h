#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/ai/evaluation_worker.h"
#include "core/browse/browse.h"
#include "core/browse/prefetch.h"
#include "core/curate/curate.h"
#include "core/decode/decode.h"
#include "core/dedup/dedup.h"
#include "core/export/export.h"
#include "core/project/project.h"
#include "core/recipe/recipe.h"
#include "core/result.h"
#include "core/settings/settings.h"
#include "core/tagging/tagging.h"

// Facade `cli` calls into. See docs/M0_Eng_Design.md for the full interface
// design; filled in module by module as core implementation progresses
// (see 任务分解/increment 顺序 in that doc).
namespace pzt::core {

using ProjectId = project::ProjectId;
using CreateProjectError = project::CreateProjectError;
using ProjectSummary = project::ProjectSummary;
using ProjectNotFoundError = project::ProjectNotFoundError;
using ImageId = project::ImageId;
using ImageInfo = project::ImageInfo;
using ScanProgressFn = project::ScanProgressFn;

using TagId = tagging::TagId;
using CreateTagError = tagging::CreateTagError;
using TagSummary = tagging::TagSummary;
using AddTagError = tagging::AddTagError;
using AddTagFailureKind = tagging::AddTagFailureKind;
using CapExceededInfo = tagging::CapExceededInfo;
using TaggedImageRef = tagging::TaggedImageRef;
using RemoveTagError = tagging::RemoveTagError;
using ReplaceTagError = tagging::ReplaceTagError;
using DeleteTagError = tagging::DeleteTagError;

using RescanSummary = project::RescanSummary;
using ImageRef = browse::ImageRef;
using BrowseTagError = browse::BrowseTagError;

// 预取/缓存环形缓冲区,见 core/browse/prefetch.h increment 6.3。跟
// ProjectSummary/ImageRef 这些纯数据类型不同,这是一个单次进程运行内持有
// 后台 jthread 的有状态组件——调用方(cli 的全键盘循环,increment 6.4)构造
// 一个实例、每次导航后调用 set_current()、渲染前调用 get()。
using PrefetchCache = browse::PrefetchCache;
using FetchError = browse::FetchError;

using SkipReason = exporting::SkipReason;
using ExportSkipped = exporting::ExportSkipped;
using ExportResult = exporting::ExportResult;
using ExportTagError = exporting::ExportTagError;
using ExportImagesError = exporting::ExportImagesError;
using ExportProgressFn = exporting::ExportProgressFn;
using ExportImageError = exporting::ExportImageError;
using ExportImageResult = exporting::ExportImageResult;

using DecodedImage = decode::DecodedImage;
using DecodeError = decode::DecodeError;
using EncodeError = decode::EncodeError;

using RecipeId = recipe::RecipeId;
using PresetSummary = recipe::PresetSummary;
using VersionSummary = recipe::VersionSummary;
using VersionParams = recipe::VersionParams;
using CreateVersionError = recipe::CreateVersionError;
using RecipeOpError = recipe::RecipeOpError;
using SetImageRecipeError = recipe::SetImageRecipeError;
using RecipeDescription = recipe::RecipeDescription;
using RenderRecipeError = recipe::RenderRecipeError;

// M3：`pzt open` 按 `:` 触发的选片辅助评估，见 core/ai/evaluation_worker.h。
// 这次固定用一个供应商(docs/M3_PRD.md 明确不做多供应商切换 UI，具体固
// 定哪一家是 cli/commands/browse.cpp 里的一行代码，不是这里的类型层面
// 决策)，类型本身整个重导出,不单独摘出一个值。
using EvaluationWorker = ai::EvaluationWorker;
using EvaluationInfo = ai::EvaluationInfo;
using Provider = ai::Provider;
// F-03：EvaluationWorker::take_last_failure() 把失败原因带出来给 cli
// 展示，cli 层需要能叫出这个类型的名字。
using EvaluationError = ai::EvaluationError;
// 综合分数/达标判断不入库，现算——CLI 展示时直接调这两个函数，不在 cli/
// 层重新实现一遍算法，见 core/ai/evaluation.h 的说明。
using ai::overall_score;
using ai::passes_gate;

// F-12：静态全局设置(供应商/dedup 参数/批量默认排除策略/界面偏好)，见
// core/settings/settings.h 的完整设计说明。cli 需要直接引用这个类型
// (存局部变量、传参),整个重导出。
using Settings = settings::Settings;
Settings load_settings();

// Opens the default global database (~/.config/pzt/pzt.db, created on first
// use) internally. `folder_path` is resolved by the caller (cli defaults it
// to cwd when omitted) - core has no notion of "current directory". M2:
// on_progress reports RAW preview cache generation progress, see
// core/project/project.h.
Result<ProjectId, CreateProjectError> create_project(const std::string& name,
                                                      const std::string& folder_path,
                                                      bool support_raw = false,
                                                      ScanProgressFn on_progress = nullptr);

std::vector<ProjectSummary> list_projects();

// Lookup helpers backing the two `pzt open` forms - by explicit name, or by
// cwd (cli resolves cwd itself and passes it in; core has no cwd concept).
std::optional<ProjectId> find_project_by_name(const std::string& name);
std::optional<ProjectId> find_project_by_root_path(const std::string& path);

// Stub for this increment: just re-fetches the project's summary. Real
// browse-session state lands with the browse module.
Result<ProjectSummary, ProjectNotFoundError> open_project(ProjectId id);

Result<void, ProjectNotFoundError> archive_project(ProjectId id);

// Cascades the project's images/tags/image_tags rows via the schema's
// ON DELETE CASCADE. Never touches files on disk.
Result<void, ProjectNotFoundError> delete_project(ProjectId id);

// 按 id 取一张图片的完整信息(含 file_size),供 increment 6.4.2 的信息栏
// 渲染当前图片的 metadata 用。
std::optional<ImageInfo> get_image(ImageId image_id);

// M4：把"项目内相对路径"翻译成 image_id——headless 命令按路径寻址图片
// (路径对人和脚本都比内存里的 image_id 稳定),见 docs/M4_Eng_Design.md
// "headless 命令面设计"一节。底层 project::find_image_by_path 已经在
// core/tests/project_test.cpp 里被当作测试 helper 广泛覆盖,这里只是转
// 调一层开默认库的门面,零逻辑,不单独测(跟 get_image/evaluated_image_ids
// 这些门面一样,不能拿默认库直接测)。
std::optional<ImageId> find_image_by_path(ProjectId project_id, const std::string& relative_path);

// F-07：批量版 get_image,只回答"这些图片里哪些已经有评估结果"——一条
// IN 查询,不是对每张图各开一次连接。见 core/project/project.h 的说明。
std::unordered_set<ImageId> evaluated_image_ids(const std::vector<ImageId>& image_ids);

Result<TagId, CreateTagError> create_tag(ProjectId project_id, const std::string& name,
                                          std::optional<std::int64_t> cap, bool is_ordered);
std::vector<TagSummary> list_tags(ProjectId project_id);
std::optional<TagId> find_tag_by_name(ProjectId project_id, const std::string& name);
std::vector<TagSummary> tags_for_image(ImageId image_id);

// F-26/F-09：给定一批图片 id，返回其中打了 tag_id 的子集，一条批量查
// 询——见 core/tagging/tagging.h 的说明。
std::unordered_set<ImageId> images_with_tag(const std::vector<ImageId>& image_ids, TagId tag_id);

Result<void, AddTagError> add_tag(ImageId image_id, TagId tag_id);
Result<void, RemoveTagError> remove_tag(ImageId image_id, TagId tag_id);
Result<void, ReplaceTagError> replace_tag_entry(TagId tag_id, ImageId old_image, ImageId new_image);
Result<void, DeleteTagError> delete_tag(TagId tag_id);
TagId ensure_reject_tag(ProjectId project_id);

// M3：近似重复检测，见 docs/M3_Dedup_Eng_Design.md"core/dedup：编排层"一
// 节。project_id 只用来找 duplicate 标签所在的项目(标签按项目隔离)，
// 不代表扫描范围——扫描范围是 image_ids，可以是整个项目也可以是一个子
// 集(比如某个标签下的图片)，由调用方(`/dedup` 控制台命令的
// handle_dedup_command)自己解析好再传进来。真正的编排逻辑在
// core::dedup::find_and_tag_duplicates(db::Database&, ...)里，这里只是
// 开默认库转调一层，方便单元测试指向临时测试库。
using DedupSummary = dedup::DedupSummary;
// F-08：time_window_seconds/hash_threshold 默认 10/5(等价旧行为)，
// handle_dedup_command 显式传 Settings.dedup_time_window_seconds/
// dedup_hash_threshold。
Result<DedupSummary, ProjectNotFoundError> find_and_tag_duplicates(
    ProjectId project_id, const std::vector<ImageId>& image_ids, int time_window_seconds = 10,
    int hash_threshold = 5, dedup::DedupProgressFn on_progress = nullptr);

// M4：策展挑图，见 docs/M4_Eng_Design.md 第三节。跟上面的 dedup 门面同一
// 个模式：开默认库转调 curate::curate。门面刻意不叫 curate——
// pzt::core::curate 已经是命名空间名，两者同名会冲突，跟 dedup 命名空间
// 配 find_and_tag_duplicates 门面名是同一个理由。curate::curate 本身不
// 读 Settings，time_window_seconds/hash_threshold 由调用方(`pzt curate`
// 命令)从 Settings.curate_time_window_seconds/curate_hash_threshold 显
// 式传入。
using CurateResult = curate::CurateResult;
CurateResult curate_images(ProjectId project_id, std::optional<TagId> candidate_scope, int count,
                            int time_window_seconds, int hash_threshold);

// 补录项目建好之后新增到磁盘上、但还不在 images 表里的文件；prune(默认
// true)时还会清掉磁盘上已消失的文件对应的记录(级联清掉标签),见
// core/project/project.h 里 rescan_project 的说明。
Result<RescanSummary, ProjectNotFoundError> rescan_project(ProjectId project_id, bool prune = true,
                                                             bool support_raw = false,
                                                             ScanProgressFn on_progress = nullptr);

std::vector<ImageRef> list_images(ProjectId project_id);
std::optional<ImageId> next_image(const std::vector<ImageRef>& images,
                                   std::optional<ImageId> current_id);
std::optional<ImageId> prev_image(const std::vector<ImageRef>& images,
                                   std::optional<ImageId> current_id);
std::optional<ImageId> next_untagged(const std::vector<ImageRef>& images,
                                      std::optional<ImageId> current_id);
std::optional<ImageId> prev_untagged(const std::vector<ImageRef>& images,
                                      std::optional<ImageId> current_id);
Result<std::vector<ImageRef>, BrowseTagError> filter_by_tag(TagId tag_id);

// M2：on_progress 汇报 RAW 图片全量解码的进度（纯 JPEG 批次不触发），见
// core/export/export.h。RawDecodeFn 不在门面层暴露——那是测试用的依赖注
// 入点，cli 不需要覆盖真实的 raw::decode_full。
//
// F-26：include_reject/include_dup 默认 false(排除废片/重复)，调用方
// (cmd_export、handle_g_export_flow)传入 Settings.export_reject/
// export_dup，语义见 core/export/export.h 的说明。
Result<ExportResult, ExportTagError> export_tag(TagId tag_id, const std::string& output_folder,
                                                 ExportProgressFn on_progress = nullptr,
                                                 bool include_reject = false, bool include_dup = false);

// 导出 cmd_open 里"当前 active filter 范围"这批图片(g 层筛选 ∘ 控制台
// 二级筛选叠加之后的结果)，不是按标签查——见 core/export/export.h 的
// export_images 说明。include_reject/include_dup 的"目标本身就是废
// 片/重复"例外由调用方(cmd_open)折算好再传进来。
Result<ExportResult, ExportImagesError> export_images(ProjectId project_id,
                                                        const std::vector<ImageId>& image_ids,
                                                        const std::string& output_folder,
                                                        ExportProgressFn on_progress = nullptr,
                                                        bool include_reject = false,
                                                        bool include_dup = false);

// 导出单张图片，不需要标签——`pzt open` 里按 `e` 键"就导出当前这张"专
// 用，见 core/export/export.h。
Result<ExportImageResult, ExportImageError> export_image(ImageId image_id,
                                                           const std::string& output_folder,
                                                           ExportProgressFn on_progress = nullptr);

// 纯粹的"字节 -> 像素"操作,不碰数据库。见 core/decode/decode.h。
Result<DecodedImage, DecodeError> decode_jpeg_file(const std::string& path);
Result<DecodedImage, DecodeError> resize_rgba(const DecodedImage& src, int target_width,
                                               int target_height);

// M2：culling 预览的统一解码入口，core/decode 和 core/raw 之间唯一的"胶
// 水"函数——按扩展名分发：.jpg/.jpeg 走 decode_jpeg_file(不变)；.dng/.raf
// 走 raw::extract_embedded_jpeg_bytes + decode::decode_jpeg_bytes(内嵌预览
// 提取，不触发 LibRaw 全量解码)。调用方(core::browse::PrefetchCache 的
// decode_fn)传的 path 已经由 browse 层按 kind/preview_cache_path 决定好
// 该传哪个了——kind="raw" 且缓存有效时，调用方直接传缓存文件的路径（本
// 身就是 .jpg，这个函数按扩展名走的还是第一条分支，不需要知道"这是缓存
// 文件"这件事）；只有缓存缺失时才会传原始 .dng/.raf 路径，走这里的兜底
// 分支。
Result<DecodedImage, DecodeError> decode_preview_file(const std::string& path);

// M1 increment 4:像素编码回 JPEG 文件，供导出烘焙和 `pzt color-debug` 用。
Result<void, EncodeError> encode_jpeg_file(const DecodedImage& img, const std::string& path,
                                            double quality = 0.9);

// 内置预设/version,见 core/recipe/recipe.h。两个函数内部都会先确保内置预
// 设已经播种(幂等),调用方不需要单独调用一次"初始化"。increment 1 只有
// 只读查询,version 的创建/改名/软删除留到 increment 2。
std::vector<PresetSummary> list_presets();
std::vector<VersionSummary> list_versions(RecipeId preset_id);

// increment 2:version 的增删改，见 core/recipe/recipe.h 里错误取值的说明。
Result<RecipeId, CreateVersionError> create_version(RecipeId preset_id,
                                                     std::optional<std::string> name,
                                                     VersionParams params);
Result<void, RecipeOpError> rename_version(RecipeId version_id, const std::string& new_name);
Result<void, RecipeOpError> delete_version(RecipeId version_id);

// increment 3:图片 ↔ recipe 关联。recipe_id = nullopt 清除(Origin)。
Result<void, SetImageRecipeError> set_image_recipe(ImageId image_id,
                                                    std::optional<RecipeId> recipe_id);
std::optional<RecipeId> get_image_recipe(ImageId image_id);

// 供 `pzt open` 信息栏显示用,见 core/recipe/recipe.h。
std::optional<RecipeDescription> describe_recipe(RecipeId recipe_id);

// increment 4:给一张已解码的图片应用 recipe。thread_count=1 用于预览
// (同步),导出烘焙传 hardware_concurrency()。见 core/recipe/recipe.h。
Result<DecodedImage, RenderRecipeError> render(const DecodedImage& src, RecipeId recipe_id,
                                                unsigned thread_count = 1);

}  // namespace pzt::core
