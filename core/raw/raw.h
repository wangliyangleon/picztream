#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/decode/decode.h"
#include "core/result.h"

// LibRaw 封装模块。见 docs/M2_Eng_Design.md "core/api 接口设计"。纯 C++
// 函数式接口(不是有状态类),风格上跟 core/decode 对齐("路径/字节进,像素
// 出",同步调用,调用方决定要不要放到线程里跑)。不碰数据库、不知道
// recipe/kind/配对是什么——那些是 core::project/core::export 的职责。跟
// core/decode 互不依赖,两者之间"按扩展名分发"这层胶水逻辑放在 core/api.cpp。
//
// M2 increment 1:这一步只接线 CMake(LibRaw 通过 pkg-config 接入,含
// libomp 头文件路径的手动修补,见 core/CMakeLists.txt 注释)、把这两个函数
// 的骨架编译链接进 core 静态库,验证依赖能正确解析——函数体目前只是最小化
// 地触碰一次 LibRaw API 证明链接是真的生效,不是 increment 2 要落地的真实
// 提取/解码逻辑,调用方不应该依赖这一步的返回值有任何实际意义。
namespace pzt::core::raw {

enum class RawError {
  FileNotFound,
  DecodeFailed,  // 文件读到了，但 LibRaw 打不开/解不出来，或内嵌预览不是 JPEG 格式
};

// unpack_thumb() + dcraw_make_mem_thumb()，只返回内嵌预览的原始 JPEG 字节，
// 不解码成像素——像素解码复用 core::decode::decode_jpeg_bytes。
Result<std::vector<std::uint8_t>, RawError> extract_embedded_jpeg_bytes(const std::string& path);

// open_file -> unpack -> dcraw_process(output_bps=8, use_camera_wb=1,
// output_color=1/*sRGB*/) -> dcraw_make_mem_image，转换成
// core::decode::DecodedImage 的 RGBA 字节布局直接返回，供
// core::recipe::render 无缝消费。
Result<decode::DecodedImage, RawError> decode_full(const std::string& path);

}  // namespace pzt::core::raw
