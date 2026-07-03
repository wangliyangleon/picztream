#include <doctest.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/decode/decode.h"

namespace fs = std::filesystem;
using pzt::core::decode::decode_jpeg_file;
using pzt::core::decode::DecodeError;

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
