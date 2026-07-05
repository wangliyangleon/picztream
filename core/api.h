#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/browse/browse.h"
#include "core/browse/prefetch.h"
#include "core/decode/decode.h"
#include "core/export/export.h"
#include "core/project/project.h"
#include "core/result.h"
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

using LinkMode = exporting::LinkMode;
using ExportSkipped = exporting::ExportSkipped;
using ExportResult = exporting::ExportResult;
using ExportTagError = exporting::ExportTagError;

using DecodedImage = decode::DecodedImage;
using DecodeError = decode::DecodeError;

// Opens the default global database (~/.config/pzt/pzt.db, created on first
// use) internally. `folder_path` is resolved by the caller (cli defaults it
// to cwd when omitted) - core has no notion of "current directory".
Result<ProjectId, CreateProjectError> create_project(const std::string& name,
                                                      const std::string& folder_path);

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

// 给 cli 调试标签命令把"图片相对路径"翻译成内部 id 用。
std::optional<ImageId> find_image_by_path(ProjectId project_id, const std::string& relative_path);

// 按 id 取一张图片的完整信息(含 file_size),供 increment 6.4.2 的信息栏
// 渲染当前图片的 metadata 用。
std::optional<ImageInfo> get_image(ImageId image_id);

Result<TagId, CreateTagError> create_tag(ProjectId project_id, const std::string& name,
                                          std::optional<std::int64_t> cap, bool is_ordered);
std::vector<TagSummary> list_tags(ProjectId project_id);
std::optional<TagId> find_tag_by_name(ProjectId project_id, const std::string& name);
std::vector<TagSummary> tags_for_image(ImageId image_id);

Result<void, AddTagError> add_tag(ImageId image_id, TagId tag_id);
Result<void, RemoveTagError> remove_tag(ImageId image_id, TagId tag_id);
Result<void, ReplaceTagError> replace_tag_entry(TagId tag_id, ImageId old_image, ImageId new_image);
Result<void, DeleteTagError> delete_tag(TagId tag_id);
TagId ensure_reject_tag(ProjectId project_id);

// 补录项目建好之后新增到磁盘上、但还不在 images 表里的文件；prune(默认
// true)时还会清掉磁盘上已消失的文件对应的记录(级联清掉标签),见
// core/project/project.h 里 rescan_project 的说明。
Result<RescanSummary, ProjectNotFoundError> rescan_project(ProjectId project_id, bool prune = true);

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

Result<ExportResult, ExportTagError> export_tag(TagId tag_id, const std::string& output_folder,
                                                 LinkMode link_mode = LinkMode::Copy);

// 纯粹的"字节 -> 像素"操作,不碰数据库。见 core/decode/decode.h。
Result<DecodedImage, DecodeError> decode_jpeg_file(const std::string& path);
Result<DecodedImage, DecodeError> resize_rgba(const DecodedImage& src, int target_width,
                                               int target_height);

}  // namespace pzt::core
