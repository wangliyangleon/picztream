#include "core/raw/raw.h"

#include <libraw/libraw.h>

#include <filesystem>

namespace pzt::core::raw {

namespace {

namespace fs = std::filesystem;

}  // namespace

// unpack_thumb() + dcraw_make_mem_thumb()：只读取内嵌预览的原始 JPEG 字节，
// 不跑去马赛克。type 不是 LIBRAW_IMAGE_JPEG（极少数机型内嵌位图缩略图）时
// 归为 DecodeFailed——spikes/libraw_probe 已经确认徕卡/富士两种测试机型
// 都是 JPEG 类型，M2 不为位图分支投入实现，真机测试遇到例外再处理（见
// docs/M2_Eng_Design.md"风险与待确认问题"）。
Result<std::vector<std::uint8_t>, RawError> extract_embedded_jpeg_bytes(const std::string& path) {
  if (!fs::exists(path)) {
    return Result<std::vector<std::uint8_t>, RawError>::Err(RawError::FileNotFound);
  }

  LibRaw proc;
  if (proc.open_file(path.c_str()) != LIBRAW_SUCCESS) {
    return Result<std::vector<std::uint8_t>, RawError>::Err(RawError::DecodeFailed);
  }
  if (proc.unpack_thumb() != LIBRAW_SUCCESS) {
    return Result<std::vector<std::uint8_t>, RawError>::Err(RawError::DecodeFailed);
  }

  int err = 0;
  libraw_processed_image_t* thumb = proc.dcraw_make_mem_thumb(&err);
  if (!thumb) {
    return Result<std::vector<std::uint8_t>, RawError>::Err(RawError::DecodeFailed);
  }
  if (thumb->type != LIBRAW_IMAGE_JPEG) {
    LibRaw::dcraw_clear_mem(thumb);
    return Result<std::vector<std::uint8_t>, RawError>::Err(RawError::DecodeFailed);
  }

  std::vector<std::uint8_t> bytes(thumb->data, thumb->data + thumb->data_size);
  LibRaw::dcraw_clear_mem(thumb);
  return Result<std::vector<std::uint8_t>, RawError>::Ok(std::move(bytes));
}

namespace {

// open_file -> unpack -> dcraw_process(output_bps=8, use_camera_wb=1,
// output_color=1/sRGB[, half_size]) -> dcraw_make_mem_image。LibRaw 内部完
// 成白平衡（as-shot）+ 去马赛克 + 色彩矩阵 + gamma，直接吐 8-bit sRGB，这
// 里只做一次字节布局转换：LibRaw 输出是 3 字节/像素紧凑排列
// (R,G,B,R,G,B,...)，decode::DecodedImage 是 4 字节/像素(R,G,B,跳过的第
// 4 字节)，对齐 core/decode/decode.cpp 用的 kCGImageAlphaNoneSkipLast +
// kCGBitmapByteOrder32Big 约定——第 4 字节不携带任何语义，下游(encode_
// jpeg_file/resize_rgba)也用同样的标志位读取，不关心这个字节的值。
// decode_full/decode_preview 共用这一份实现，只是 half_size 参数不同。
Result<decode::DecodedImage, RawError> decode(const std::string& path, bool half_size) {
  if (!fs::exists(path)) {
    return Result<decode::DecodedImage, RawError>::Err(RawError::FileNotFound);
  }

  LibRaw proc;
  if (proc.open_file(path.c_str()) != LIBRAW_SUCCESS) {
    return Result<decode::DecodedImage, RawError>::Err(RawError::DecodeFailed);
  }
  proc.imgdata.params.use_camera_wb = 1;
  proc.imgdata.params.output_bps = 8;
  proc.imgdata.params.output_color = 1;  // sRGB
  proc.imgdata.params.half_size = half_size ? 1 : 0;

  if (proc.unpack() != LIBRAW_SUCCESS) {
    return Result<decode::DecodedImage, RawError>::Err(RawError::DecodeFailed);
  }
  if (proc.dcraw_process() != LIBRAW_SUCCESS) {
    return Result<decode::DecodedImage, RawError>::Err(RawError::DecodeFailed);
  }

  int err = 0;
  libraw_processed_image_t* img = proc.dcraw_make_mem_image(&err);
  if (!img) {
    return Result<decode::DecodedImage, RawError>::Err(RawError::DecodeFailed);
  }
  if (img->colors != 3 || img->bits != 8) {
    // dcraw_process 已经显式要求 output_bps=8，这里理论上不该发生；防御性
    // 地拒绝而不是按错误的通道数瞎读内存。
    LibRaw::dcraw_clear_mem(img);
    return Result<decode::DecodedImage, RawError>::Err(RawError::DecodeFailed);
  }

  decode::DecodedImage out;
  out.width = img->width;
  out.height = img->height;
  const std::size_t pixel_count =
      static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
  out.rgba.resize(pixel_count * 4);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    out.rgba[i * 4 + 0] = img->data[i * 3 + 0];
    out.rgba[i * 4 + 1] = img->data[i * 3 + 1];
    out.rgba[i * 4 + 2] = img->data[i * 3 + 2];
    out.rgba[i * 4 + 3] = 0;
  }

  LibRaw::dcraw_clear_mem(img);
  return Result<decode::DecodedImage, RawError>::Ok(std::move(out));
}

}  // namespace

Result<decode::DecodedImage, RawError> decode_full(const std::string& path) {
  return decode(path, /*half_size=*/false);
}

Result<decode::DecodedImage, RawError> decode_preview(const std::string& path) {
  return decode(path, /*half_size=*/true);
}

std::optional<std::int64_t> read_capture_time(const std::string& path) {
  if (!fs::exists(path)) return std::nullopt;

  LibRaw proc;
  if (proc.open_file(path.c_str()) != LIBRAW_SUCCESS) return std::nullopt;

  time_t ts = proc.imgdata.other.timestamp;
  if (ts == 0) return std::nullopt;  // LibRaw 用 0 表示"这个字段没有数据"
  return static_cast<std::int64_t>(ts);
}

}  // namespace pzt::core::raw
