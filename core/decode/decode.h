#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"

// JPEG 解码模块。见 docs/M0_Eng_Design.md increment 6.1。复用
// spikes/kitty_latency_probe/ 里已经验证过的 ImageIO/CoreGraphics 解码路径
// (CGImageSourceCreateWithData -> CGImageSourceCreateImageAtIndex ->
// CGBitmapContext device RGB -> CGContextDrawImage)。色彩管理沿用
// CoreGraphics 默认的 ColorSync 匹配行为，不额外做 ICC 读取/转换（"待确认
// 问题"里记录的结论）。
//
// 这一步只负责"字节 -> 像素"，不关心延迟（预取/缓存的调度才是真正的延迟
// 敏感路径，属于 increment 6.3），也不关心并发（同步调用，调用方决定要不
// 要放到 jthread 里跑）。
namespace pzt::core::decode {

struct DecodedImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;  // 4 字节/像素，行优先，device RGB
};

enum class DecodeError {
  FileNotFound,
  DecodeFailed,  // 文件读到了，但不是 ImageIO 能识别/解码的图像内容
};

Result<DecodedImage, DecodeError> decode_jpeg_file(const std::string& path);

// M2：decode_jpeg_file 拆出来的后半段（字节 -> 像素），供
// core::raw::extract_embedded_jpeg_bytes 拿到的内嵌预览字节复用，不用重
// 新实现一遍 CGImageSource 逻辑。
Result<DecodedImage, DecodeError> decode_jpeg_bytes(const std::vector<std::uint8_t>& bytes);

// 把已经解码的图片缩放成正好 target_width x target_height(不保持长宽比——
// 调用方负责用长宽比正确的目标尺寸调用这个函数,这里只管缩放本身)。目标
// 尺寸已经不小于原图时直接返回原图的拷贝,不做无意义的重采样。
//
// 用途:全键盘浏览循环里,每次把解码出来的原始分辨率图片(可能几 MB 到近
// 十 MB 的 RGBA)整个丢给终端,终端自己临时文件读取 + 解码 + 缩放显示,是
// 真机测试确认过的实际卡顿来源之一——先在这里缩小到终端面板大致能显示的
// 尺寸,再传给终端,大幅减少终端侧要处理的数据量。
Result<DecodedImage, DecodeError> resize_rgba(const DecodedImage& src, int target_width,
                                               int target_height);

// M1 increment 4:把像素编码回 JPEG 文件——M0 从来没有这个需求(导出只是
// 复制/软链原始文件字节)，这次给应用了 recipe 的图片导出烘焙用。用
// CGImageDestinationCreateWithURL + kCGImageDestinationLossyCompressionQuality，
// 跟 core/tests/decode_test.cpp 测试夹具里已经用过的
// CGBitmapContext -> CGImageDestination 路径一致，这次提升成生产函数。
enum class EncodeError {
  EncodeFailed,
};

Result<void, EncodeError> encode_jpeg_file(const DecodedImage& img, const std::string& path,
                                            double quality = 0.9);

// EXIF DateTimeOriginal，只读容器 metadata 不解码像素
// （CGImageSourceCopyPropertiesAtIndex 实测几毫秒量级）。文件打不开、没
// 有 EXIF、没有这个字段，统一返回 nullopt——跟 raw::read_capture_time 是
// 同一种"没有这个信息不是错误"的处理方式。
std::optional<std::int64_t> read_jpeg_capture_time(const std::string& path);

}  // namespace pzt::core::decode
