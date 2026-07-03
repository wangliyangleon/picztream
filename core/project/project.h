#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/result.h"

// 项目导入模块。见 docs/M0_Eng_Design.md "core/project" 模块划分与
// core/api 接口设计。这些函数接收一个已经打开的 Database&，方便单元测试
// 指向临时测试库而不碰真实的 ~/.config/pzt/pzt.db；cli 实际使用的入口是
// core/api.h 里委托到这里、内部自己打开默认库的门面函数。
namespace pzt::core::project {

using ProjectId = std::int64_t;

enum class CreateProjectError {
  NameAlreadyExists,
  NoImagesFound,
};

struct ProjectSummary {
  ProjectId id;
  std::string name;
  std::string root_path;
  std::int64_t image_count;
  bool archived;
};

// 递归扫描 folder_path 下所有 .jpg/.jpeg（大小写不敏感），写入 images 表。
// 名字已存在或扫描不到任何 JPEG 时返回对应错误，不创建项目。
Result<ProjectId, CreateProjectError> create_project(db::Database& db,
                                                      const std::string& name,
                                                      const std::string& folder_path);

// 未归档项目在前，归档项目排最后；同组内按名字排序。
std::vector<ProjectSummary> list_projects(db::Database& db);

enum class ProjectNotFoundError {
  NotFound,
};

std::optional<ProjectId> find_project_by_name(db::Database& db, const std::string& name);
std::optional<ProjectId> find_project_by_root_path(db::Database& db, const std::string& path);

// "打开"这个 increment 里只是重新查一遍摘要返回给 cli 打印，不产生真正的
// 浏览会话状态——那是浏览模块（increment 4）要引入的东西。
Result<ProjectSummary, ProjectNotFoundError> open_project(db::Database& db, ProjectId id);

// 幂等：对已归档项目重复调用只是更新时间戳，不当错误处理。
Result<void, ProjectNotFoundError> archive_project(db::Database& db, ProjectId id);

// 级联清除该项目的 images/tags/image_tags（靠 schema 的 ON DELETE CASCADE），
// 不触碰磁盘上的原始文件。
Result<void, ProjectNotFoundError> delete_project(db::Database& db, ProjectId id);

using ImageId = std::int64_t;

struct ImageInfo {
  ImageId id;
  ProjectId project_id;
  std::string file_path;
  std::string file_name;
};

// 给 cli 调试命令把"图片相对路径"翻译成内部 id 用。
std::optional<ImageId> find_image_by_path(db::Database& db, ProjectId project_id,
                                           const std::string& relative_path);

// 给标签模块校验"这张图属于哪个项目"用。
std::optional<ImageInfo> get_image(db::Database& db, ImageId id);

struct RescanSummary {
  std::int64_t added_count;
  std::int64_t total_count;
};

// 复用 create_project 内部的扫描逻辑，只把磁盘上有、但 images 表里还没有
// 的文件插进去；不删除、不处理磁盘上已经消失的文件（那样做有可能悄悄丢失
// 已经打好的标签，风险比"新照片暂时看不到"大得多，不在这个函数的范围内）。
Result<RescanSummary, ProjectNotFoundError> rescan_project(db::Database& db, ProjectId id);

}  // namespace pzt::core::project
