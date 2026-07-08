#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/decode/decode.h"
#include "core/project/project.h"
#include "core/raw/raw.h"
#include "core/result.h"
#include "core/tagging/tagging.h"

// 导出模块。完整设计(行为流程、文件命名规则、冲突消歧规则)见
// docs/M0_Eng_Design.md "导出" 小节。命名空间叫 exporting 而不是 export，
// 因为 export 是 C++ 关键字。
namespace pzt::core::exporting {

using project::ImageId;
using tagging::TagId;

// 跳过原因。用结构化枚举而不是字符串——core 层不产出面向用户展示的文
// 本，这跟项目里其它错误类型（CreateProjectError、RecipeOpError、
// DecodeError 等）的约定一致，转成人话是 cli 的职责（见 cli/i18n 的
// export_skip_reason）。M2：RawDecodeFailed 单独区分开，跟"普通 JPEG 解
// 码失败"(DecodeFailed)不是一回事，跳过原因里能直接看出是哪条路径出的问题。
enum class SkipReason { SourceMissing, DecodeFailed, RenderFailed, EncodeFailed, RawDecodeFailed };

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

// M2：导出批量处理进度——只有这批图片里确实有 kind="raw" 的才会被调用，
// 纯 JPEG 批次不触发（呼应 core::project::ScanProgressFn 同一个设计取
// 舍：不为不相关的场景打印"0/0"这种没有意义的输出）。跟 ScanProgressFn
// 类型结构一样但语义上是两件事（一个是 new/rescan 的预览缓存生成进度，
// 一个是 export 的全量解码进度），各自独立声明，不共享。
using ExportProgressFn = std::function<void(int done, int total)>;

// M2：kind="raw" 图片的全量解码函数，默认指向真实的 raw::decode_full。
// 测试注入假函数验证路由逻辑，不需要真的链接调用 LibRaw（跟
// browse::PrefetchCache 的 DecodeFn 是同一个注入模式）。
using RawDecodeFn = std::function<Result<decode::DecodedImage, raw::RawError>(const std::string&)>;

// 有序标签用 {零填充序号}_{原文件名}(宽度取 max(2, 本次导出总数的位数)),
// 无序标签直接用原文件名;命名冲突时在扩展名前追加 _2、_3……直到不冲突;
// 源文件在磁盘上缺失时跳过并记录原因，不中断其余图片的导出。
//
// M1:应用了 recipe 的图片走"解码全分辨率原图 -> core::recipe::render
// (多线程)-> core::decode::encode_jpeg_file"这条烘焙路径，输出文件体
// 现处理后的效果，不是原始文件的直接拷贝；没有应用 recipe 的图片继续
// 保持字节级不变的复制行为。解码/渲染/编码任一步失败都归入 `skipped`
// （复用现有的"源文件缺失"跳过机制），不中断其余图片的导出。
//
// M2：kind="raw" 的图片无论有没有 recipe 都要走 `raw_decode_fn` 全量解
// 码 -> (有 recipe 才 render) -> encode_jpeg_file，输出文件名的扩展名会
// 被换成 .jpg；`on_progress` 汇报这部分的解码进度。
//
// 曾经有一个 `link_mode` 参数(复制 vs 软链)，M2 时移除了——应用了
// recipe/kind="raw" 的图片输出永远是新生成的文件，没有"原始字节"可软
// 链，唯一还会被软链模式影响的场景("纯 JPEG + 无 recipe")覆盖面太窄，
// 想不出实际用途，直接删掉比保留一个只在一种场景下生效的选项更简单。
Result<ExportResult, ExportTagError> export_tag(db::Database& db, TagId tag_id,
                                                 const std::string& output_folder,
                                                 ExportProgressFn on_progress = nullptr,
                                                 RawDecodeFn raw_decode_fn = raw::decode_full);

enum class ExportImageError {
  ImageNotFound,
  IoError,  // 目标文件夹无法创建/写入，跟 ExportTagError::IoError 是同一类问题
};

struct ExportImageResult {
  bool exported;                          // false 时看 skip_reason，Result 本身仍是 Ok——
                                           // 源文件缺失/解码失败是"这一张没导出成"，不是整
                                           // 个调用失败，跟 export_tag 对单张图片失败的处理
                                           // 保持同一个语义
  std::optional<SkipReason> skip_reason;  // exported==false 时有值
  std::string output_path;                // exported==true 时是写出的绝对路径
  bool created_output_folder;
};

// 导出单张图片，不需要标签——`pzt open` 里按 `e` 键"就导出当前这张"这个
// 场景专用，跟 export_tag 共用同一套 kind × 有无 recipe 路由逻辑(见
// export.cpp 里的私有 write_one_export)，但没有"有序编号"这个概念(单张
// 导出直接用原文件名)。
Result<ExportImageResult, ExportImageError> export_image(db::Database& db, ImageId image_id,
                                                           const std::string& output_folder,
                                                           ExportProgressFn on_progress = nullptr,
                                                           RawDecodeFn raw_decode_fn = raw::decode_full);

}  // namespace pzt::core::exporting
