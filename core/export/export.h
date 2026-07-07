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

// 跳过原因。用结构化枚举而不是字符串——core 层不产出面向用户展示的文
// 本，这跟项目里其它错误类型（CreateProjectError、RecipeOpError、
// DecodeError 等）的约定一致，转成人话是 cli 的职责（见 cli/i18n 的
// export_skip_reason）。
enum class SkipReason { SourceMissing, DecodeFailed, RenderFailed, EncodeFailed };

struct ExportSkipped {
  ImageId image_id;
  std::string file_name;
  SkipReason reason;
};

struct ExportResult {
  int exported_count;
  std::vector<ExportSkipped> skipped;
  bool created_output_folder;  // true 表示 output_folder 之前不存在，这次调用新建的
};

enum class ExportTagError {
  TagNotFound,
  IoError,  // 目标文件夹无法创建/写入(权限不足、路径上某一段已经是个普通文件等)
};

// 有序标签用 {零填充序号}_{原文件名}(宽度取 max(2, 本次导出总数的位数)),
// 无序标签直接用原文件名;命名冲突时在扩展名前追加 _2、_3……直到不冲突;
// 源文件在磁盘上缺失时跳过并记录原因，不中断其余图片的导出。
//
// M1:应用了 recipe 的图片走"解码全分辨率原图 -> core::recipe::render
// (多线程)-> core::decode::encode_jpeg_file"这条烘焙路径，输出文件体
// 现处理后的效果，不是原始文件的直接拷贝；没有应用 recipe 的图片继续
// 保持字节级不变的复制/软链行为。`link_mode` 对烘焙路径没有意义——输出
// 本来就是新生成的文件，没有"原始字节"可以软链，这类图片不管
// `link_mode` 是什么都会落地成真实文件，这是对既有 `--link` 语义的一
// 个自然限制，不是 bug。解码/渲染/编码任一步失败都归入 `skipped`（复用
// 现有的"源文件缺失"跳过机制），不中断其余图片的导出。
Result<ExportResult, ExportTagError> export_tag(db::Database& db, TagId tag_id,
                                                 const std::string& output_folder,
                                                 LinkMode link_mode = LinkMode::Copy);

}  // namespace pzt::core::exporting
