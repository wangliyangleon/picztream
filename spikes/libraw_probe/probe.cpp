// Throwaway spike: M2 Phase 0 technical validation. Measures two LibRaw
// operations against real DNG (Leica Q3) / RAF (Fujifilm X-T5, X-Trans)
// files: (1) embedded-preview extraction (unpack_thumb + dcraw_make_mem_thumb)
// - the culling-path operation, must be cheap and must not trigger a full
// demosaic; (2) full decode (unpack + dcraw_process + dcraw_make_mem_image,
// output_bps=8, use_camera_wb=1, output_color=1/sRGB) - the processing-path
// operation, only triggered by `pzt export`, not latency-budget-constrained
// the way culling is. Not production code; see README.md and results.md.
//
// Build: clang++ -std=c++20 -O2 -fexperimental-library -o probe probe.cpp \
//          $(pkg-config --cflags --libs libraw)
// Run:   ./probe <raw1> [raw2 ...]

#include <libraw/libraw.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

namespace {

const char* image_type_name(LibRaw_image_formats type) {
  switch (type) {
    case LIBRAW_IMAGE_JPEG:
      return "JPEG";
    case LIBRAW_IMAGE_BITMAP:
      return "BITMAP";
    default:
      return "OTHER";
  }
}

// unpack_thumb() + dcraw_make_mem_thumb()：只读取内嵌预览，不跑去马赛克。
// 每次重新 open_file，避免前一次调用的内部状态影响计时。
double time_thumb_extract(const std::string& path, int* out_width, int* out_height,
                           const char** out_type, unsigned* out_data_size) {
  LibRaw proc;
  if (proc.open_file(path.c_str()) != LIBRAW_SUCCESS) {
    std::fprintf(stderr, "open_file failed: %s\n", path.c_str());
    return -1;
  }

  auto t0 = clk::now();
  int ret = proc.unpack_thumb();
  double elapsed = -1;
  if (ret == LIBRAW_SUCCESS) {
    int err = 0;
    libraw_processed_image_t* thumb = proc.dcraw_make_mem_thumb(&err);
    auto t1 = clk::now();
    elapsed = ms(t1 - t0).count();
    if (thumb) {
      *out_width = thumb->width;
      *out_height = thumb->height;
      *out_type = image_type_name(thumb->type);
      *out_data_size = thumb->data_size;
      LibRaw::dcraw_clear_mem(thumb);
    }
  } else {
    std::fprintf(stderr, "unpack_thumb failed (%d): %s\n", ret, path.c_str());
  }
  proc.recycle();
  return elapsed;
}

// unpack() + dcraw_process() + dcraw_make_mem_image()：完整去马赛克路径，
// 8-bit sRGB + as-shot 白平衡（M2_Eng_Design 已确认的输出格式）。
double time_full_decode(const std::string& path, int* out_width, int* out_height,
                         int* out_colors, int* out_bits) {
  LibRaw proc;
  if (proc.open_file(path.c_str()) != LIBRAW_SUCCESS) {
    std::fprintf(stderr, "open_file failed: %s\n", path.c_str());
    return -1;
  }
  proc.imgdata.params.use_camera_wb = 1;
  proc.imgdata.params.output_bps = 8;
  proc.imgdata.params.output_color = 1;  // sRGB

  auto t0 = clk::now();
  int ret = proc.unpack();
  if (ret != LIBRAW_SUCCESS) {
    std::fprintf(stderr, "unpack failed (%d): %s\n", ret, path.c_str());
    proc.recycle();
    return -1;
  }
  ret = proc.dcraw_process();
  if (ret != LIBRAW_SUCCESS) {
    std::fprintf(stderr, "dcraw_process failed (%d): %s\n", ret, path.c_str());
    proc.recycle();
    return -1;
  }
  int err = 0;
  libraw_processed_image_t* img = proc.dcraw_make_mem_image(&err);
  auto t1 = clk::now();
  double elapsed = ms(t1 - t0).count();
  if (img) {
    *out_width = img->width;
    *out_height = img->height;
    *out_colors = img->colors;
    *out_bits = img->bits;
    LibRaw::dcraw_clear_mem(img);
  } else {
    elapsed = -1;
  }
  proc.recycle();
  return elapsed;
}

double min_of(const std::vector<double>& v) {
  double m = v[0];
  for (double x : v) m = std::min(m, x);
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <raw1> [raw2 ...]\n", argv[0]);
    return 1;
  }

  constexpr int kRepeats = 5;

  for (int i = 1; i < argc; ++i) {
    std::string path = argv[i];
    std::printf("=== %s ===\n", path.c_str());

    std::vector<double> thumb_times;
    int tw = 0, th = 0;
    const char* ttype = "?";
    unsigned tsize = 0;
    for (int r = 0; r < kRepeats; ++r) {
      double t = time_thumb_extract(path, &tw, &th, &ttype, &tsize);
      if (t < 0) {
        std::printf("  thumb extract: FAILED\n");
        break;
      }
      thumb_times.push_back(t);
    }
    if (thumb_times.size() == kRepeats) {
      std::printf("  thumb extract: %.2fms (min of %d), type=%s, %dx%d, data_size=%u bytes\n",
                  min_of(thumb_times), kRepeats, ttype, tw, th, tsize);
    }

    std::vector<double> decode_times;
    int dw = 0, dh = 0, dc = 0, db = 0;
    for (int r = 0; r < kRepeats; ++r) {
      double t = time_full_decode(path, &dw, &dh, &dc, &db);
      if (t < 0) {
        std::printf("  full decode: FAILED\n");
        break;
      }
      decode_times.push_back(t);
    }
    if (decode_times.size() == kRepeats) {
      std::printf("  full decode: %.2fms (min of %d), %dx%d, colors=%d, bits=%d\n",
                  min_of(decode_times), kRepeats, dw, dh, dc, db);
    }
  }

  return 0;
}
