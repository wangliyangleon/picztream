#pragma once

#include <cstdint>
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

}  // namespace pzt::core::decode
