#pragma once

#include <cstdint>
#include <optional>
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
// core::recipe::render 无缝消费。只用于导出（唯一需要全分辨率的场景）。
Result<decode::DecodedImage, RawError> decode_full(const std::string& path);

// 跟 decode_full 完全一样的参数（同一套白平衡/色彩矩阵/gamma 管线），额外
// 加 half_size=1：跳过完整去马赛克，直接块平均，分辨率减半但速度快很多
// （真机实测：徕卡 Bayer ~2 倍，富士 X-Trans ~40 倍——X-Trans 的完整去马
// 赛克正是全量解码慢的根源，half_size 几乎整个跳过了它）。只用于生成
// new/rescan 时的预览缓存，不能当导出结果用（分辨率不够）。
Result<decode::DecodedImage, RawError> decode_preview(const std::string& path);

// 只读文件头部/metadata，不走 unpack()/dcraw_process() 那条全量解码慢路
// 径——open_file() 本身就已经把这个信息解析出来了（实测徕卡/富士两台测
// 试机身都是 1-2ms 量级）。相机没提供、文件打不开、解析失败，统一返回
// nullopt——调用方（core::project 的排序场景）把它当"没有这个信息"处
// 理，不是需要报错的异常状态。
std::optional<std::int64_t> read_capture_time(const std::string& path);

}  // namespace pzt::core::raw
