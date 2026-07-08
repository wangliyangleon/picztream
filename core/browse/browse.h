#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/result.h"
#include "core/tagging/tagging.h"

// 浏览与过滤模块。见 docs/M0_Eng_Design.md "core/browse" 模块划分。
//
// "当前浏览到哪张图"不是这个模块的状态——真正的全键盘循环（increment 6）
// 是单次进程运行内的内存状态，调用方（cli，最终是交互循环）自己持有
// "current_id"并在每次导航后更新它，这里只提供无状态的查询/计算函数。
namespace pzt::core::browse {

using project::ImageId;
using project::ProjectId;
using tagging::TagId;

struct ImageRef {
  ImageId id;
  std::string file_path;
  std::string file_name;
  std::string kind = "jpeg";                     // "jpeg" | "raw"，见 M2_Eng_Design.md
  std::optional<std::string> preview_cache_path;  // kind="raw" 且缓存已生成时有值(绝对路径)
};

// 项目内所有图片，按 file_path 字典序排列——一个确定性的浏览顺序基准。
std::vector<ImageRef> list_images(db::Database& db, ProjectId project_id);

// current_id 为 nullopt，或者传入的 id 根本不在 images 列表里（防御性地按
// nullopt 处理），表示"还没选定，从第一张开始"。纯内存运算，不查库。到达
// 两端时循环折返（最后一张的下一张是第一张）。
std::optional<ImageId> next_image(const std::vector<ImageRef>& images,
                                   std::optional<ImageId> current_id);
std::optional<ImageId> prev_image(const std::vector<ImageRef>& images,
                                   std::optional<ImageId> current_id);

// 按循环顺序扫描，跳过已经打过至少一个标签的图片；扫描一整圈都没有未打
// 标签的图片时返回 nullopt（这是"全部处理完了"的有意义终止信号，不是错误，
// 也不会死循环）。
std::optional<ImageId> next_untagged(db::Database& db, const std::vector<ImageRef>& images,
                                      std::optional<ImageId> current_id);
std::optional<ImageId> prev_untagged(db::Database& db, const std::vector<ImageRef>& images,
                                      std::optional<ImageId> current_id);

enum class BrowseTagError {
  TagNotFound,
};

// 有序标签按 position 升序，无序标签按 tagged_at 升序——跟标签模块、导出
// 模块的排序规则保持一致。
Result<std::vector<ImageRef>, BrowseTagError> filter_by_tag(db::Database& db, TagId tag_id);

}  // namespace pzt::core::browse
