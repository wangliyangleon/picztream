#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
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

// F-26/F-09：给定一批图片 id，返回其中打了 tag_id 这个标签的子集——一条
// 批量查询，不是逐张查（跟 project::evaluated_image_ids 同一个模式）。
std::unordered_set<ImageId> images_with_tag(db::Database& db, const std::vector<ImageId>& image_ids,
                                             TagId tag_id);

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

enum class DeleteTagError {
  TagNotFound,
  SystemTagProtected,
};

// 删除标签定义本身（不是某一张图片的关联），级联清掉所有图片与它的
// image_tags 关联——靠 schema 里 image_tags.tag_id REFERENCES tags(id) ON
// DELETE CASCADE，跟 delete_project 依赖的是同一套级联机制。
//
// 跟 remove_tag 的幂等语义不同：remove_tag 删的是"一张图和一个标签的关联"
// 这一行，不存在也不算错；delete_tag 删的是标签这个实体本身，tag_id 不存
// 在是错误（TagNotFound），跟 delete_project/archive_project 现有的"实体
// 级删除，找不到就报错"约定一致，不是新规则，不是幂等操作。
//
// 系统标签在这一层也拒绝删除（SystemTagProtected），不指望"cli 的 space d
// 菜单不会把系统标签列出来"这个 UI 层面的过滤就足够安全——其它调用方不受
// 这层 UI 过滤保护。
Result<void, DeleteTagError> delete_tag(db::Database& db, TagId tag_id);

// increment 6.4.5:核心逻辑和 cli 菜单渲染共用同一个符号，不各自硬编码一遍。
constexpr const char* kRejectTagName = "废片";

// 确保这个项目里存在一个名字是"废片"的系统标签，不存在就创建。只在
// pzt new 创建项目的那一刻调用一次——项目刚创建时保证没有任何标签，不需
// 要处理"同名标签已经存在但不是系统标签"这种迁移场景。pzt open 也调用同
// 一个函数（不是单独查 find_tag_by_name 再解引用），处理"项目不是通过更
// 新后的 pzt new 建的"这种边界情况，find-or-create 本身是幂等、廉价的，
// 不需要额外的迁移/归一化逻辑。
TagId ensure_reject_tag(db::Database& db, ProjectId project_id);

// M3：近似重复检测(core/dedup)标记重复项用的系统标签，见
// docs/M3_Dedup_Eng_Design.md"core/tagging：新增 ensure_duplicate_tag"一
// 节。跟 kRejectTagName 同一个惯例——中文，用户在 pzt open 里直接看到
// 这个名字。
constexpr const char* kDuplicateTagName = "重复";

// 幂等：查不到就创建，逻辑跟 ensure_reject_tag 完全一致，只换了标签名
// 字——不共享实现，两行代码不值得为此抽一层。
TagId ensure_duplicate_tag(db::Database& db, ProjectId project_id);

}  // namespace pzt::core::tagging
