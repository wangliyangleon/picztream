#include "core/decode/decode.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <fstream>
#include <optional>

namespace pzt::core::decode {

namespace {

std::optional<std::vector<std::uint8_t>> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return std::nullopt;
  auto size = f.tellg();
  if (size < 0) return std::nullopt;
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(buf.data()), size);
  return buf;
}

}  // namespace

Result<DecodedImage, DecodeError> decode_jpeg_file(const std::string& path) {
  auto bytes = read_file(path);
  if (!bytes) return Result<DecodedImage, DecodeError>::Err(DecodeError::FileNotFound);

  CFDataRef data = CFDataCreate(nullptr, bytes->data(), static_cast<CFIndex>(bytes->size()));
  CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  if (!src) return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);

  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
  CFRelease(src);
  if (!img) return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);

  DecodedImage out;
  out.width = static_cast<int>(CGImageGetWidth(img));
  out.height = static_cast<int>(CGImageGetHeight(img));
  out.rgba.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4);

  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  const auto bitmap_info = static_cast<CGBitmapInfo>(
      static_cast<std::uint32_t>(kCGImageAlphaNoneSkipLast) |
      static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
  CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), out.width, out.height, 8, out.width * 4,
                                            cs, bitmap_info);
  if (!ctx) {
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);
  }
  CGContextDrawImage(ctx, CGRectMake(0, 0, out.width, out.height), img);

  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  CGImageRelease(img);

  return Result<DecodedImage, DecodeError>::Ok(std::move(out));
}

Result<DecodedImage, DecodeError> resize_rgba(const DecodedImage& src, int target_width,
                                               int target_height) {
  if (target_width <= 0 || target_height <= 0 || src.width <= 0 || src.height <= 0) {
    return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);
  }
  if (target_width >= src.width && target_height >= src.height) {
    return Result<DecodedImage, DecodeError>::Ok(src);  // 已经够小,不用重采样
  }

  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  const auto bitmap_info = static_cast<CGBitmapInfo>(
      static_cast<std::uint32_t>(kCGImageAlphaNoneSkipLast) |
      static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));

  // 把已有的 RGBA 像素包成一张只读 CGImage,不拷贝数据——release callback
  // 留空,src 的生命周期由调用方保证覆盖这次调用。
  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, src.rgba.data(), src.rgba.size(), nullptr);
  CGImageRef src_img = CGImageCreate(src.width, src.height, 8, 32, src.width * 4, cs, bitmap_info,
                                      provider, nullptr, false, kCGRenderingIntentDefault);
  CGDataProviderRelease(provider);
  if (!src_img) {
    CGColorSpaceRelease(cs);
    return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);
  }

  DecodedImage out;
  out.width = target_width;
  out.height = target_height;
  out.rgba.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4);

  CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), out.width, out.height, 8, out.width * 4,
                                            cs, bitmap_info);
  CGColorSpaceRelease(cs);
  if (!ctx) {
    CGImageRelease(src_img);
    return Result<DecodedImage, DecodeError>::Err(DecodeError::DecodeFailed);
  }
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  CGContextDrawImage(ctx, CGRectMake(0, 0, out.width, out.height), src_img);

  CGContextRelease(ctx);
  CGImageRelease(src_img);
  return Result<DecodedImage, DecodeError>::Ok(std::move(out));
}

}  // namespace pzt::core::decode
