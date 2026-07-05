// Throwaway one-off tool: generates a synthetic 500+ JPEG dataset for the
// M0 PRD's "browsing 500+ images with no perceptible stutter" acceptance
// criterion. Not part of the CMake build or ctest - there's no real 500-photo
// dataset available locally, so this synthesizes one, reusing the same
// CoreGraphics/ImageIO encoding approach already established in
// core/tests/decode_test.cpp's encode_solid_jpeg (avoids committing binary
// image fixtures to the repo). Unlike that test helper's tiny fixtures, this
// uses realistic phone-camera resolutions (3072x4080 / 4080x3072, alternating
// to exercise both portrait/landscape fit_within paths) so file sizes and
// decode costs are in the right ballpark - not exact, since solid-color JPEGs
// compress far smaller and decode faster than real photos (see README.md).
//
// Build: clang++ -std=c++20 -O2 -o generate generate.cpp \
//          -framework CoreGraphics -framework ImageIO -framework CoreFoundation
// Run:   ./generate <output_dir> [count]   (count defaults to 550)

#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

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
  CGContextRef ctx =
      CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4, cs, bitmap_info);
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

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <output_dir> [count]\n", argv[0]);
    return 1;
  }
  fs::path out_dir = argv[1];
  int count = argc >= 3 ? std::atoi(argv[2]) : 550;

  fs::create_directories(out_dir);

  for (int i = 0; i < count; ++i) {
    // 交替朝向,覆盖 fit_within 两种情形;每张换一个填充色,避免文件字节
    // 完全相同。
    bool portrait = (i % 2 == 0);
    int width = portrait ? 3072 : 4080;
    int height = portrait ? 4080 : 3072;
    unsigned char r = static_cast<unsigned char>((i * 37) % 256);
    unsigned char g = static_cast<unsigned char>((i * 73) % 256);
    unsigned char b = static_cast<unsigned char>((i * 113) % 256);

    auto bytes = encode_solid_jpeg(width, height, r, g, b);

    char name[64];
    std::snprintf(name, sizeof(name), "bench_%04d.jpg", i);
    std::ofstream f(out_dir / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));

    if ((i + 1) % 50 == 0 || i + 1 == count) {
      std::fprintf(stderr, "已生成 %d/%d\n", i + 1, count);
    }
  }

  std::fprintf(stderr, "完成,共 %d 张,输出到 %s\n", count, out_dir.c_str());
  return 0;
}
