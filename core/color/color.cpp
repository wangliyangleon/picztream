#include "core/color/color.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <thread>

namespace pzt::core::color {

namespace {

// 三线性插值，照抄 spikes/color_lut_probe/probe.cpp::sample_lut。
inline void sample_lut(const Lut3D& lut, float r, float g, float b, float out[3]) {
  const int n = lut.size;
  const float rs = r * (n - 1), gs = g * (n - 1), bs = b * (n - 1);
  const int r0 = static_cast<int>(rs), g0 = static_cast<int>(gs), b0 = static_cast<int>(bs);
  const int r1 = std::min(r0 + 1, n - 1), g1 = std::min(g0 + 1, n - 1),
            b1 = std::min(b0 + 1, n - 1);
  const float fr = rs - r0, fg = gs - g0, fb = bs - b0;

  auto at = [&](int ri, int gi, int bi) -> const float* {
    std::size_t idx = (static_cast<std::size_t>(ri) * n + gi) * n + bi;
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

void apply_lut_rows(decode::DecodedImage& img, const Lut3D& lut, int row_begin, int row_end) {
  for (int y = row_begin; y < row_end; ++y) {
    std::uint8_t* row = &img.rgba[static_cast<std::size_t>(y) * img.width * 4];
    for (int x = 0; x < img.width; ++x) {
      std::uint8_t* px = row + x * 4;
      float out[3];
      sample_lut(lut, px[0] / 255.f, px[1] / 255.f, px[2] / 255.f, out);
      px[0] = static_cast<std::uint8_t>(std::clamp(out[0], 0.f, 1.f) * 255.f + 0.5f);
      px[1] = static_cast<std::uint8_t>(std::clamp(out[1], 0.f, 1.f) * 255.f + 0.5f);
      px[2] = static_cast<std::uint8_t>(std::clamp(out[2], 0.f, 1.f) * 255.f + 0.5f);
    }
  }
}

void apply_adjustments_rows(decode::DecodedImage& img, const AdjustParams& params, int row_begin,
                             int row_end) {
  const float wb_gain_r = static_cast<float>(1.0 + params.wb_shift_r / 100.0);
  const float wb_gain_b = static_cast<float>(1.0 + params.wb_shift_b / 100.0);
  const float highlights_f = static_cast<float>(params.highlights / 100.0);
  const float shadows_f = static_cast<float>(params.shadows / 100.0);
  const float whites_f = static_cast<float>(params.whites / 100.0);
  const float blacks_f = static_cast<float>(params.blacks / 100.0);
  const float contrast_gain = static_cast<float>(1.0 + params.contrast / 100.0);
  const float sat_gain = static_cast<float>(1.0 + params.saturation / 100.0);

  for (int y = row_begin; y < row_end; ++y) {
    std::uint8_t* row = &img.rgba[static_cast<std::size_t>(y) * img.width * 4];
    for (int x = 0; x < img.width; ++x) {
      std::uint8_t* px = row + x * 4;
      float r = px[0] / 255.f, g = px[1] / 255.f, b = px[2] / 255.f;
      float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
      float highlight_weight = std::clamp((luminance - 0.5f) / 0.5f, 0.f, 1.f);
      float shadow_weight = std::clamp((0.5f - luminance) / 0.5f, 0.f, 1.f);
      // 比 highlight_weight/shadow_weight 窄的加权区间(0.2 阈值),只影响
      // 真正逼近纯白/纯黑的像素——摄影里"影调端点"和"影调恢复"两个不同
      // 概念的常规区分,见 core/color/color.h 的公式注释。
      float white_weight = std::clamp((luminance - 0.8f) / 0.2f, 0.f, 1.f);
      float black_weight = std::clamp((0.2f - luminance) / 0.2f, 0.f, 1.f);
      float delta = highlights_f * highlight_weight + shadows_f * shadow_weight +
                    whites_f * white_weight + blacks_f * black_weight;

      float r1 = std::clamp(r * wb_gain_r + delta, 0.f, 1.f);
      float g1 = std::clamp(g + delta, 0.f, 1.f);
      float b1 = std::clamp(b * wb_gain_b + delta, 0.f, 1.f);

      float r2 = std::clamp((r1 - 0.5f) * contrast_gain + 0.5f, 0.f, 1.f);
      float g2 = std::clamp((g1 - 0.5f) * contrast_gain + 0.5f, 0.f, 1.f);
      float b2 = std::clamp((b1 - 0.5f) * contrast_gain + 0.5f, 0.f, 1.f);

      float luma2 = 0.299f * r2 + 0.587f * g2 + 0.114f * b2;
      float r3 = std::clamp(luma2 + (r2 - luma2) * sat_gain, 0.f, 1.f);
      float g3 = std::clamp(luma2 + (g2 - luma2) * sat_gain, 0.f, 1.f);
      float b3 = std::clamp(luma2 + (b2 - luma2) * sat_gain, 0.f, 1.f);

      px[0] = static_cast<std::uint8_t>(r3 * 255.f + 0.5f);
      px[1] = static_cast<std::uint8_t>(g3 * 255.f + 0.5f);
      px[2] = static_cast<std::uint8_t>(b3 * 255.f + 0.5f);
    }
  }
}

inline float grain_noise(int x, int y) {
  std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
                     static_cast<std::uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= (h >> 16);
  return (static_cast<float>(h & 0xFFFFu) / 65535.f) * 2.f - 1.f;  // [-1, 1]
}

// amount=1.0 时的最大加减幅度,约 30/255——第一版工作假设,真机验证时按实
// 际观感调整,不是摄影级精确校准。
constexpr float kGrainMaxIntensity = 0.12f;

void apply_grain_rows(decode::DecodedImage& img, float amount, int row_begin, int row_end) {
  const float scale = amount * kGrainMaxIntensity;
  for (int y = row_begin; y < row_end; ++y) {
    std::uint8_t* row = &img.rgba[static_cast<std::size_t>(y) * img.width * 4];
    for (int x = 0; x < img.width; ++x) {
      std::uint8_t* px = row + x * 4;
      float n = grain_noise(x, y) * scale;
      px[0] = static_cast<std::uint8_t>(std::clamp(px[0] / 255.f + n, 0.f, 1.f) * 255.f + 0.5f);
      px[1] = static_cast<std::uint8_t>(std::clamp(px[1] / 255.f + n, 0.f, 1.f) * 255.f + 0.5f);
      px[2] = static_cast<std::uint8_t>(std::clamp(px[2] / 255.f + n, 0.f, 1.f) * 255.f + 0.5f);
    }
  }
}

// thread_count=1 时直接同步跑完，不额外起线程；>1 时按行切分到
// std::jthread，析构时自动 join——照抄
// spikes/color_lut_probe/probe.cpp::time_jthread 里验证过的切分写法。
void run_parallel_rows(int height, unsigned thread_count, const std::function<void(int, int)>& fn) {
  if (thread_count <= 1) {
    fn(0, height);
    return;
  }
  std::vector<std::jthread> pool;
  pool.reserve(thread_count);
  int chunk = (height + static_cast<int>(thread_count) - 1) / static_cast<int>(thread_count);
  for (unsigned t = 0; t < thread_count; ++t) {
    int begin = static_cast<int>(t) * chunk;
    int end = std::min(height, begin + chunk);
    if (begin >= end) break;
    pool.emplace_back([&fn, begin, end] { fn(begin, end); });
  }
  // pool 在这里析构，每个 jthread 自动 join。
}

}  // namespace

void apply_lut(decode::DecodedImage& img, const Lut3D& lut, unsigned thread_count) {
  run_parallel_rows(img.height, thread_count,
                     [&](int b, int e) { apply_lut_rows(img, lut, b, e); });
}

void apply_adjustments(decode::DecodedImage& img, const AdjustParams& params,
                        unsigned thread_count) {
  run_parallel_rows(img.height, thread_count,
                     [&](int b, int e) { apply_adjustments_rows(img, params, b, e); });
}

void apply_grain(decode::DecodedImage& img, float amount, unsigned thread_count) {
  if (amount <= 0.f) return;
  run_parallel_rows(img.height, thread_count,
                     [&](int b, int e) { apply_grain_rows(img, amount, b, e); });
}

}  // namespace pzt::core::color
