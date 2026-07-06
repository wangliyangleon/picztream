// Throwaway spike: M1 Phase 0 technical validation. Measures the per-pixel
// cost of two color-recipe operations - simple white balance (cheap,
// per-channel multiply) and 3D LUT trilinear interpolation (the generic
// mechanism behind real color-grading "looks") - at preview and full-sensor
// resolutions, single-threaded vs jthread-parallelized. Answers the open
// design question in docs/Roadmap.md's Milestone 1 description: is
// "downsampled sync preview + full-res async queue" actually justified by
// the numbers, or is scalar/jthread already fast enough that the design can
// be simpler? Not production code; see README.md and results.md.
//
// Build: clang++ -std=c++20 -O2 -fexperimental-library -o probe probe.cpp \
//          -framework CoreGraphics -framework ImageIO -framework CoreFoundation
// Run:   ./probe <jpeg1> [jpeg2 ...]

#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

namespace {

std::vector<unsigned char> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("cannot open " + path);
  auto size = f.tellg();
  std::vector<unsigned char> buf(static_cast<size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(buf.data()), size);
  return buf;
}

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // 4 bytes/pixel, R,G,B,(unused alpha)
};

// 跟 core/decode/decode.cpp、kitty_latency_probe 用的是同一条 ImageIO/
// CoreGraphics 解码路径,这里独立抄一份而不是链接 core 静态库,保持这个
// spike 完全自包含、跟 CMake 构建链无关(跟 kitty_latency_probe 同样的
// 惯例)。
Image decode_jpeg(const std::vector<unsigned char>& bytes) {
  CFDataRef data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
  CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
  if (!src) throw std::runtime_error("CGImageSourceCreateWithData failed");
  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
  if (!img) throw std::runtime_error("CGImageSourceCreateImageAtIndex failed");

  Image out;
  out.width = static_cast<int>(CGImageGetWidth(img));
  out.height = static_cast<int>(CGImageGetHeight(img));
  out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);

  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx =
      CGBitmapContextCreate(out.rgba.data(), out.width, out.height, 8, out.width * 4, cs,
                             kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
  CGContextDrawImage(ctx, CGRectMake(0, 0, out.width, out.height), img);

  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  CGImageRelease(img);
  CFRelease(src);
  CFRelease(data);
  return out;
}

// --- 3D LUT ---------------------------------------------------------------

struct Lut3D {
  int n = 0;
  std::vector<float> data;  // n*n*n*3,值域 [0,1]
};

// 故意做一个有实际扭曲的合成"配方"(轻微 S 型对比度 + 暖色/冷色偏移),不
// 是恒等映射——恒等映射会让 CPU 分支预测器/预取器在这次合成测试里"猜对"
// 太多次,不代表真实调色 LUT 那种不规则的输出分布,会把耗时测得偏乐观。
Lut3D make_lut(int n) {
  Lut3D lut{n, std::vector<float>(static_cast<size_t>(n) * n * n * 3)};
  for (int r = 0; r < n; ++r) {
    for (int g = 0; g < n; ++g) {
      for (int b = 0; b < n; ++b) {
        float rf = static_cast<float>(r) / (n - 1);
        float gf = static_cast<float>(g) / (n - 1);
        float bf = static_cast<float>(b) / (n - 1);
        float rf2 = std::clamp(rf + 0.08f * std::sin(rf * 3.14159f), 0.f, 1.f);
        float gf2 = std::clamp(gf + 0.04f * std::sin((gf - 0.3f) * 3.14159f), 0.f, 1.f);
        float bf2 = std::clamp(bf - 0.06f * std::sin(bf * 3.14159f), 0.f, 1.f);
        size_t idx = (static_cast<size_t>(r) * n + g) * n + b;
        lut.data[idx * 3 + 0] = rf2;
        lut.data[idx * 3 + 1] = gf2;
        lut.data[idx * 3 + 2] = bf2;
      }
    }
  }
  return lut;
}

inline void sample_lut(const Lut3D& lut, float r, float g, float b, float out[3]) {
  const int n = lut.n;
  const float rs = r * (n - 1), gs = g * (n - 1), bs = b * (n - 1);
  const int r0 = static_cast<int>(rs), g0 = static_cast<int>(gs), b0 = static_cast<int>(bs);
  const int r1 = std::min(r0 + 1, n - 1), g1 = std::min(g0 + 1, n - 1),
            b1 = std::min(b0 + 1, n - 1);
  const float fr = rs - r0, fg = gs - g0, fb = bs - b0;

  auto at = [&](int ri, int gi, int bi) -> const float* {
    size_t idx = (static_cast<size_t>(ri) * n + gi) * n + bi;
    return &lut.data[idx * 3];
  };
  const float* c000 = at(r0, g0, b0);
  const float* c001 = at(r0, g0, b1);
  const float* c010 = at(r0, g1, b0);
  const float* c011 = at(r0, g1, b1);
  const float* c100 = at(r1, g0, b0);
  const float* c101 = at(r1, g0, b1);
  const float* c110 = at(r1, g1, b0);
  const float* c111 = at(r1, g1, b1);

  for (int ch = 0; ch < 3; ++ch) {
    float c00 = c000[ch] * (1 - fb) + c001[ch] * fb;
    float c01 = c010[ch] * (1 - fb) + c011[ch] * fb;
    float c10 = c100[ch] * (1 - fb) + c101[ch] * fb;
    float c11 = c110[ch] * (1 - fb) + c111[ch] * fb;
    float c0 = c00 * (1 - fg) + c01 * fg;
    float c1 = c10 * (1 - fg) + c11 * fg;
    out[ch] = c0 * (1 - fr) + c1 * fr;
  }
}

void apply_lut_rows(std::vector<uint8_t>& rgba, int width, const Lut3D& lut, int row_begin,
                     int row_end) {
  for (int y = row_begin; y < row_end; ++y) {
    uint8_t* row = &rgba[static_cast<size_t>(y) * width * 4];
    for (int x = 0; x < width; ++x) {
      uint8_t* px = row + x * 4;
      float out[3];
      sample_lut(lut, px[0] / 255.f, px[1] / 255.f, px[2] / 255.f, out);
      px[0] = static_cast<uint8_t>(std::clamp(out[0], 0.f, 1.f) * 255.f + 0.5f);
      px[1] = static_cast<uint8_t>(std::clamp(out[1], 0.f, 1.f) * 255.f + 0.5f);
      px[2] = static_cast<uint8_t>(std::clamp(out[2], 0.f, 1.f) * 255.f + 0.5f);
    }
  }
}

// --- 白平衡(对照组:简单逐像素乘加) -----------------------------------

void apply_white_balance_rows(std::vector<uint8_t>& rgba, int width, float gr, float gg, float gb,
                               int row_begin, int row_end) {
  for (int y = row_begin; y < row_end; ++y) {
    uint8_t* row = &rgba[static_cast<size_t>(y) * width * 4];
    for (int x = 0; x < width; ++x) {
      uint8_t* px = row + x * 4;
      px[0] = static_cast<uint8_t>(std::min(255.f, px[0] * gr));
      px[1] = static_cast<uint8_t>(std::min(255.f, px[1] * gg));
      px[2] = static_cast<uint8_t>(std::min(255.f, px[2] * gb));
    }
  }
}

// --- 计时辅助 ---------------------------------------------------------

using RowFn = std::function<void(int, int)>;

double time_single_thread(int height, const RowFn& fn, int reps) {
  double best = 1e18;
  for (int i = 0; i < reps; ++i) {
    auto t0 = clk::now();
    fn(0, height);
    best = std::min(best, ms(clk::now() - t0).count());
  }
  return best;
}

double time_jthread(int height, const RowFn& fn, unsigned thread_count, int reps) {
  double best = 1e18;
  for (int i = 0; i < reps; ++i) {
    auto t0 = clk::now();
    {
      std::vector<std::jthread> pool;
      pool.reserve(thread_count);
      int chunk = (height + static_cast<int>(thread_count) - 1) / static_cast<int>(thread_count);
      for (unsigned t = 0; t < thread_count; ++t) {
        int begin = static_cast<int>(t) * chunk;
        int end = std::min(height, begin + chunk);
        if (begin >= end) break;
        pool.emplace_back([&fn, begin, end] { fn(begin, end); });
      }
      // jthread 析构时自动 join,不需要手动 join() 这一步。
    }
    best = std::min(best, ms(clk::now() - t0).count());
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <jpeg1> [jpeg2 ...]\n", argv[0]);
    return 1;
  }

  unsigned hw = std::thread::hardware_concurrency();
  std::printf("hardware_concurrency() = %u\n\n", hw);

  Lut3D lut17 = make_lut(17);
  Lut3D lut33 = make_lut(33);

  std::printf(
      "%-16s %10s %8s | %10s %10s | %10s %10s | %10s %10s\n", "file", "px", "reps", "wb_1thread",
      "wb_Nthread", "lut17_1t", "lut17_Nt", "lut33_1t", "lut33_Nt");

  for (int i = 1; i < argc; ++i) {
    std::string path = argv[i];
    Image img;
    try {
      img = decode_jpeg(read_file(path));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
      continue;
    }

    const int reps = 5;
    auto wb_fn = [&](int b, int e) {
      apply_white_balance_rows(img.rgba, img.width, 1.15f, 1.0f, 0.85f, b, e);
    };
    auto lut17_fn = [&](int b, int e) { apply_lut_rows(img.rgba, img.width, lut17, b, e); };
    auto lut33_fn = [&](int b, int e) { apply_lut_rows(img.rgba, img.width, lut33, b, e); };

    double wb_1t = time_single_thread(img.height, wb_fn, reps);
    double wb_nt = time_jthread(img.height, wb_fn, hw, reps);
    double lut17_1t = time_single_thread(img.height, lut17_fn, reps);
    double lut17_nt = time_jthread(img.height, lut17_fn, hw, reps);
    double lut33_1t = time_single_thread(img.height, lut33_fn, reps);
    double lut33_nt = time_jthread(img.height, lut33_fn, hw, reps);

    std::printf("%-16s %10zu %8d | %10.2f %10.2f | %10.2f %10.2f | %10.2f %10.2f\n",
                path.c_str(), static_cast<size_t>(img.width) * img.height, reps, wb_1t, wb_nt,
                lut17_1t, lut17_nt, lut33_1t, lut33_nt);
  }
  return 0;
}
