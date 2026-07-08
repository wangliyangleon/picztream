#include "core/decode/decode.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <ctime>
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

Result<DecodedImage, DecodeError> decode_jpeg_bytes(const std::vector<std::uint8_t>& bytes) {
  CFDataRef data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
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

Result<DecodedImage, DecodeError> decode_jpeg_file(const std::string& path) {
  auto bytes = read_file(path);
  if (!bytes) return Result<DecodedImage, DecodeError>::Err(DecodeError::FileNotFound);
  return decode_jpeg_bytes(*bytes);
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

Result<void, EncodeError> encode_jpeg_file(const DecodedImage& img, const std::string& path,
                                            double quality) {
  if (img.width <= 0 || img.height <= 0) {
    return Result<void, EncodeError>::Err(EncodeError::EncodeFailed);
  }

  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  const auto bitmap_info = static_cast<CGBitmapInfo>(
      static_cast<std::uint32_t>(kCGImageAlphaNoneSkipLast) |
      static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
  // 跟 resize_rgba 一样,把已有的 RGBA 像素包成一张只读 CGImage,不拷贝数据。
  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, img.rgba.data(), img.rgba.size(), nullptr);
  CGImageRef cg_img = CGImageCreate(img.width, img.height, 8, 32, img.width * 4, cs, bitmap_info,
                                     provider, nullptr, false, kCGRenderingIntentDefault);
  CGDataProviderRelease(provider);
  CGColorSpaceRelease(cs);
  if (!cg_img) return Result<void, EncodeError>::Err(EncodeError::EncodeFailed);

  CFStringRef cf_path = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
  CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cf_path, kCFURLPOSIXPathStyle, false);
  CFRelease(cf_path);
  if (!url) {
    CGImageRelease(cg_img);
    return Result<void, EncodeError>::Err(EncodeError::EncodeFailed);
  }

  CGImageDestinationRef dest =
      CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, nullptr);
  CFRelease(url);
  if (!dest) {
    CGImageRelease(cg_img);
    return Result<void, EncodeError>::Err(EncodeError::EncodeFailed);
  }

  CFNumberRef quality_value = CFNumberCreate(nullptr, kCFNumberDoubleType, &quality);
  const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
  const void* values[] = {quality_value};
  CFDictionaryRef options =
      CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                          &kCFTypeDictionaryValueCallBacks);
  CFRelease(quality_value);

  CGImageDestinationAddImage(dest, cg_img, options);
  bool ok = CGImageDestinationFinalize(dest);

  CFRelease(options);
  CFRelease(dest);
  CGImageRelease(cg_img);

  if (!ok) return Result<void, EncodeError>::Err(EncodeError::EncodeFailed);
  return Result<void, EncodeError>::Ok();
}

std::optional<std::int64_t> read_jpeg_capture_time(const std::string& path) {
  CFStringRef cf_path = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
  CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cf_path, kCFURLPOSIXPathStyle, false);
  CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(cf_path);
  CFRelease(url);
  if (!src) return std::nullopt;

  CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr);
  CFRelease(src);
  if (!props) return std::nullopt;

  std::optional<std::int64_t> result;
  auto exif = static_cast<CFDictionaryRef>(
      CFDictionaryGetValue(props, kCGImagePropertyExifDictionary));
  if (exif) {
    auto dto = static_cast<CFStringRef>(
        CFDictionaryGetValue(exif, kCGImagePropertyExifDateTimeOriginal));
    if (dto) {
      char buf[32];
      if (CFStringGetCString(dto, buf, sizeof(buf), kCFStringEncodingUTF8)) {
        // EXIF 固定格式 "YYYY:MM:DD HH:MM:SS"，没有时区信息——用 mktime()
        // 按本地时区解释，不是 timegm()/UTC。这不是随便选的：实测过
        // LibRaw 的 imgdata.other.timestamp 对同一个字符串就是用 mktime()
        // 语义算出来的（真机验证时用真实富士 RAF 核实过，两条路径读到
        // 的原始字符串一致，转出来的 epoch 也必须一致，否则同一个项目
        // 里 RAW 和真实相机 JPEG 的 captured_at 会有一个时区偏移量的系统
        // 性错位，混合排序时序会乱）。跨相机比较时如果两台相机所在时区
        // 确实不同，排序可能有小误差，这是目前接受的已知简化。
        std::tm tm{};
        if (strptime(buf, "%Y:%m:%d %H:%M:%S", &tm)) {
          tm.tm_isdst = -1;  // 交给 mktime 自己判断有没有夏令时，不要瞎猜
          result = static_cast<std::int64_t>(mktime(&tm));
        }
      }
    }
  }
  CFRelease(props);
  return result;
}

}  // namespace pzt::core::decode
