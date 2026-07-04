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

}  // namespace pzt::core::decode
