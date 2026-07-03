#pragma once

#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/result.h"
#include "core/tagging/tagging.h"

// 导出模块。完整设计(行为流程、文件命名规则、冲突消歧规则)见
// docs/M0_Eng_Design.md "导出" 小节。命名空间叫 exporting 而不是 export，
// 因为 export 是 C++ 关键字。
namespace pzt::core::exporting {

using project::ImageId;
using tagging::TagId;

enum class LinkMode { Copy, Symlink };

struct ExportSkipped {
  ImageId image_id;
  std::string file_name;
  std::string reason;  // 目前主要是"源文件缺失"，字符串类型本身不限制未来扩展
};

struct ExportResult {
  int exported_count;
  std::vector<ExportSkipped> skipped;
};

enum class ExportTagError {
  TagNotFound,
};

// 有序标签用 {零填充序号}_{原文件名}(宽度取 max(2, 本次导出总数的位数)),
// 无序标签直接用原文件名;命名冲突时在扩展名前追加 _2、_3……直到不冲突;
// 源文件在磁盘上缺失时跳过并记录原因，不中断其余图片的导出。
Result<ExportResult, ExportTagError> export_tag(db::Database& db, TagId tag_id,
                                                 const std::string& output_folder,
                                                 LinkMode link_mode = LinkMode::Copy);

}  // namespace pzt::core::exporting
