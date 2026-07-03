#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/browse/browse.h"
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

using TagId = tagging::TagId;
using CreateTagError = tagging::CreateTagError;
using TagSummary = tagging::TagSummary;
using AddTagError = tagging::AddTagError;
using AddTagFailureKind = tagging::AddTagFailureKind;
using CapExceededInfo = tagging::CapExceededInfo;
using TaggedImageRef = tagging::TaggedImageRef;
using RemoveTagError = tagging::RemoveTagError;
using ReplaceTagError = tagging::ReplaceTagError;

using RescanSummary = project::RescanSummary;
using ImageRef = browse::ImageRef;
using BrowseTagError = browse::BrowseTagError;

using LinkMode = exporting::LinkMode;
using ExportSkipped = exporting::ExportSkipped;
using ExportResult = exporting::ExportResult;
using ExportTagError = exporting::ExportTagError;

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

Result<TagId, CreateTagError> create_tag(ProjectId project_id, const std::string& name,
                                          std::optional<std::int64_t> cap, bool is_ordered);
std::vector<TagSummary> list_tags(ProjectId project_id);
std::optional<TagId> find_tag_by_name(ProjectId project_id, const std::string& name);

Result<void, AddTagError> add_tag(ImageId image_id, TagId tag_id);
Result<void, RemoveTagError> remove_tag(ImageId image_id, TagId tag_id);
Result<void, ReplaceTagError> replace_tag_entry(TagId tag_id, ImageId old_image, ImageId new_image);

// 补录项目建好之后新增到磁盘上、但还不在 images 表里的文件；只增不减,见
// core/project/project.h 里 rescan_project 的说明。
Result<RescanSummary, ProjectNotFoundError> rescan_project(ProjectId project_id);

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

}  // namespace pzt::core
