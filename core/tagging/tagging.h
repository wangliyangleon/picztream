#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/result.h"

// 标签模块。见 docs/M0_Eng_Design.md "core/tagging" 模块划分。这些函数接收
// 一个已经打开的 Database&，跟 core/project 一样的模式；cli 实际使用的入口
// 是 core/api.h 里的门面函数。
namespace pzt::core::tagging {

using TagId = std::int64_t;
using project::ImageId;
using project::ProjectId;

enum class CreateTagError {
  NameAlreadyExists,
};

struct TagSummary {
  TagId id;
  std::string name;
  std::optional<std::int64_t> cap;
  bool is_ordered;
  bool is_system;
  std::int64_t tagged_count;
};

// is_system 目前只是让调用方能建出系统标签用的通用字段——具体"废片"这个
// 系统标签什么时候被创建，是 increment 6 才要决定的产品行为，不是这个函数
// 的事。
Result<TagId, CreateTagError> create_tag(db::Database& db, ProjectId project_id,
                                          const std::string& name, std::optional<std::int64_t> cap,
                                          bool is_ordered, bool is_system = false);

std::vector<TagSummary> list_tags(db::Database& db, ProjectId project_id);
std::optional<TagId> find_tag_by_name(db::Database& db, ProjectId project_id,
                                       const std::string& name);

// 反方向查询："这张图当前打了哪些标签"（list_tags 是"这个项目有哪些标
// 签"，filter_by_tag/ordered_entries 内部是"这个标签下有哪些图"）。increment
// 6.4.2 的信息栏要用它显示当前图片的标签列表。按名字排序，不存在的
// image_id 直接返回空列表，不当错误处理——跟"这张图没有标签"是同一回事。
std::vector<TagSummary> tags_for_image(db::Database& db, ImageId image_id);

struct TaggedImageRef {
  ImageId image_id;
  std::string file_name;
};

// 有序标签按 position 升序排列，无序标签按 tagged_at 升序——跟导出模块用的
// 排序规则保持一致。
struct CapExceededInfo {
  std::int64_t cap;
  std::vector<TaggedImageRef> existing_entries;
};

enum class AddTagFailureKind {
  TagNotFound,
  ImageNotFound,
  ProjectMismatch,  // image 和 tag 不属于同一个项目
  CapExceeded,
};

struct AddTagError {
  AddTagFailureKind kind;
  std::optional<CapExceededInfo> cap_info;  // 仅当 kind == CapExceeded 时有值
};

// 幂等：图片已经打过这个标签，再打一次直接算成功，不重复插入、不报错。
Result<void, AddTagError> add_tag(db::Database& db, ImageId image_id, TagId tag_id);

enum class RemoveTagError {
  TagNotFound,
  ImageNotFound,
};

// 幂等：图片本来就没这个标签，删除也直接算成功。
Result<void, RemoveTagError> remove_tag(db::Database& db, ImageId image_id, TagId tag_id);

enum class ReplaceTagError {
  TagNotFound,
  OldImageNotTagged,
  NewImageNotFound,
};

// 新图片接管旧图片的 position（无序标签则是 NULL 接 NULL），不是追加到末尾
// ——对应 PRD"替换已有的某一张"这个语义，而不是"删一张、加一张到最后"。
Result<void, ReplaceTagError> replace_tag_entry(db::Database& db, TagId tag_id, ImageId old_image,
                                                 ImageId new_image);

}  // namespace pzt::core::tagging
