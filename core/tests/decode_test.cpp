#include <doctest.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/decode/decode.h"

namespace fs = std::filesystem;
using pzt::core::decode::decode_jpeg_file;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::DecodeError;
using pzt::core::decode::encode_jpeg_file;
using pzt::core::decode::read_jpeg_capture_time;
using pzt::core::decode::resize_rgba;

namespace {

fs::path fresh_tmp_dir() {
  auto dir = fs::temp_directory_path() / "pzt_test" / "decode";
  fs::create_directories(dir);
  return dir;
}

// 生成一张纯色 JPEG 用作测试夹具,避免往仓库里塞二进制图片资源。用
// CGImageDestination（decode.cpp 用的 CGImageSource 的逆过程）现场编码。
std::vector<unsigned char> encode_solid_jpeg(int width, int height, unsigned char r,
                                              unsigned char g, unsigned char b) {
  std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = r;
    pixels[i + 1] = g;
    pixels[i + 2] = b;
    pixels[i + 3] = 255;
  }

  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  const auto bitmap_info = static_cast<CGBitmapInfo>(
      static_cast<std::uint32_t>(kCGImageAlphaNoneSkipLast) |
      static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
  CGContextRef ctx = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4, cs, bitmap_info);
  CGImageRef img = CGBitmapContextCreateImage(ctx);

  CFMutableDataRef out_data = CFDataCreateMutable(nullptr, 0);
  CGImageDestinationRef dest =
      CGImageDestinationCreateWithData(out_data, CFSTR("public.jpeg"), 1, nullptr);
  CGImageDestinationAddImage(dest, img, nullptr);
  CGImageDestinationFinalize(dest);

  std::vector<unsigned char> bytes(static_cast<std::size_t>(CFDataGetLength(out_data)));
  std::memcpy(bytes.data(), CFDataGetBytePtr(out_data), bytes.size());

  CFRelease(dest);
  CFRelease(out_data);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return bytes;
}

fs::path write_jpeg_fixture(const std::string& name, int width, int height, unsigned char r,
                             unsigned char g, unsigned char b) {
  auto bytes = encode_solid_jpeg(width, height, r, g, b);
  auto path = fresh_tmp_dir() / name;
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

// read_jpeg_capture_time 测试专用：跟 encode_solid_jpeg 几乎一样，只是
// CGImageDestinationAddImage 的 properties 参数从 nullptr 换成带 EXIF
// DateTimeOriginal 的字典，现场编码一张真的带拍摄时间的测试 JPEG——这样
// 就能对着真实 EXIF 格式做精确断言，不用只测"没崩溃"。
fs::path write_jpeg_with_exif_date(const std::string& name, const std::string& exif_date) {
  std::vector<unsigned char> pixels(4 * 4 * 4, 128);
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  const auto bitmap_info = static_cast<CGBitmapInfo>(
      static_cast<std::uint32_t>(kCGImageAlphaNoneSkipLast) |
      static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
  CGContextRef ctx = CGBitmapContextCreate(pixels.data(), 4, 4, 8, 4 * 4, cs, bitmap_info);
  CGImageRef img = CGBitmapContextCreateImage(ctx);

  CFStringRef date_str = CFStringCreateWithCString(nullptr, exif_date.c_str(), kCFStringEncodingUTF8);
  const void* exif_keys[] = {kCGImagePropertyExifDateTimeOriginal};
  const void* exif_values[] = {date_str};
  CFDictionaryRef exif_dict =
      CFDictionaryCreate(nullptr, exif_keys, exif_values, 1, &kCFTypeDictionaryKeyCallBacks,
                          &kCFTypeDictionaryValueCallBacks);
  const void* prop_keys[] = {kCGImagePropertyExifDictionary};
  const void* prop_values[] = {exif_dict};
  CFDictionaryRef props =
      CFDictionaryCreate(nullptr, prop_keys, prop_values, 1, &kCFTypeDictionaryKeyCallBacks,
                          &kCFTypeDictionaryValueCallBacks);

  CFMutableDataRef out_data = CFDataCreateMutable(nullptr, 0);
  CGImageDestinationRef dest =
      CGImageDestinationCreateWithData(out_data, CFSTR("public.jpeg"), 1, nullptr);
  CGImageDestinationAddImage(dest, img, props);
  CGImageDestinationFinalize(dest);

  auto path = fresh_tmp_dir() / name;
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(CFDataGetBytePtr(out_data)),
          static_cast<std::streamsize>(CFDataGetLength(out_data)));

  CFRelease(props);
  CFRelease(exif_dict);
  CFRelease(date_str);
  CFRelease(dest);
  CFRelease(out_data);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return path;
}

}  // namespace

TEST_CASE("decode_jpeg_file decodes dimensions and near-exact solid color") {
  auto path = write_jpeg_fixture("solid_red.jpg", 16, 12, 200, 40, 40);

  auto result = decode_jpeg_file(path.string());
  REQUIRE(result.ok());
  const auto& img = result.value();
  CHECK(img.width == 16);
  CHECK(img.height == 12);
  REQUIRE(img.rgba.size() == static_cast<std::size_t>(16 * 12 * 4));

  // JPEG 有损压缩，纯色图允许小幅偏差，但不应该面目全非；alpha 通道恒为
  // 255（decode.cpp 用 kCGImageAlphaNoneSkipLast，不携带真实透明度）。
  CHECK(std::abs(static_cast<int>(img.rgba[0]) - 200) <= 8);
  CHECK(std::abs(static_cast<int>(img.rgba[1]) - 40) <= 8);
  CHECK(std::abs(static_cast<int>(img.rgba[2]) - 40) <= 8);
  CHECK(img.rgba[3] == 255);
}

TEST_CASE("decode_jpeg_file reports FileNotFound for a missing path") {
  auto result = decode_jpeg_file("/nonexistent/pzt_test_missing.jpg");
  REQUIRE(!result.ok());
  CHECK(result.error() == DecodeError::FileNotFound);
}

TEST_CASE("decode_jpeg_file reports DecodeFailed for a non-image file") {
  auto path = fresh_tmp_dir() / "not_an_image.jpg";
  std::ofstream f(path, std::ios::binary);
  f << "this is not a jpeg";

  auto result = decode_jpeg_file(path.string());
  REQUIRE(!result.ok());
  CHECK(result.error() == DecodeError::DecodeFailed);
}

namespace {

DecodedImage make_solid(int width, int height, unsigned char r, unsigned char g,
                         unsigned char b) {
  DecodedImage img;
  img.width = width;
  img.height = height;
  img.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i + 0] = r;
    img.rgba[i + 1] = g;
    img.rgba[i + 2] = b;
    img.rgba[i + 3] = 255;
  }
  return img;
}

}  // namespace

TEST_CASE("resize_rgba shrinks dimensions and preserves a solid color") {
  auto src = make_solid(400, 300, 10, 200, 100);

  auto result = resize_rgba(src, 40, 30);
  REQUIRE(result.ok());
  const auto& out = result.value();
  CHECK(out.width == 40);
  CHECK(out.height == 30);
  REQUIRE(out.rgba.size() == static_cast<std::size_t>(40 * 30 * 4));

  // 高质量插值缩放纯色图,理论上应该逐像素精确保留原色。
  CHECK(out.rgba[0] == 10);
  CHECK(out.rgba[1] == 200);
  CHECK(out.rgba[2] == 100);
  CHECK(out.rgba[3] == 255);
}

TEST_CASE("resize_rgba is a no-op copy when the target is already at least as big") {
  auto src = make_solid(20, 10, 5, 6, 7);

  auto result = resize_rgba(src, 100, 100);
  REQUIRE(result.ok());
  CHECK(result.value().width == 20);   // 尺寸没变
  CHECK(result.value().height == 10);
  CHECK(result.value().rgba == src.rgba);
}

TEST_CASE("resize_rgba reports DecodeFailed for non-positive dimensions") {
  auto src = make_solid(10, 10, 1, 2, 3);
  CHECK(!resize_rgba(src, 0, 10).ok());
  CHECK(!resize_rgba(src, 10, 0).ok());
  CHECK(!resize_rgba(src, -1, 10).ok());
}

TEST_CASE("encode_jpeg_file writes a file that decode_jpeg_file can read back") {
  auto src = make_solid(40, 30, 10, 200, 100);
  auto path = fresh_tmp_dir() / "encoded.jpg";

  auto encoded = encode_jpeg_file(src, path.string());
  REQUIRE(encoded.ok());
  REQUIRE(fs::exists(path));

  auto decoded = decode_jpeg_file(path.string());
  REQUIRE(decoded.ok());
  CHECK(decoded.value().width == 40);
  CHECK(decoded.value().height == 30);
  // 有损压缩,允许小幅偏差,不要求逐字节相等。
  CHECK(std::abs(static_cast<int>(decoded.value().rgba[0]) - 10) <= 5);
  CHECK(std::abs(static_cast<int>(decoded.value().rgba[1]) - 200) <= 5);
  CHECK(std::abs(static_cast<int>(decoded.value().rgba[2]) - 100) <= 5);
}

TEST_CASE("encode_jpeg_file reports EncodeFailed for non-positive dimensions") {
  DecodedImage img;
  img.width = 0;
  img.height = 0;
  auto path = fresh_tmp_dir() / "should_not_exist.jpg";
  CHECK(!encode_jpeg_file(img, path.string()).ok());
}

TEST_CASE("read_jpeg_capture_time parses EXIF DateTimeOriginal as a UTC epoch") {
  auto path = write_jpeg_with_exif_date("with_date.jpg", "2025:05:11 19:24:22");
  auto result = read_jpeg_capture_time(path.string());
  REQUIRE(result.has_value());

  std::tm expected{};
  strptime("2025:05:11 19:24:22", "%Y:%m:%d %H:%M:%S", &expected);
  CHECK(*result == static_cast<std::int64_t>(timegm(&expected)));
}

TEST_CASE("read_jpeg_capture_time returns nullopt when there's no EXIF DateTimeOriginal") {
  auto path = write_jpeg_fixture("no_date.jpg", 4, 4, 128, 128, 128);
  CHECK_FALSE(read_jpeg_capture_time(path.string()).has_value());
}

TEST_CASE("read_jpeg_capture_time returns nullopt for a missing file") {
  CHECK_FALSE(read_jpeg_capture_time("/nonexistent/pzt_test_missing.jpg").has_value());
}
