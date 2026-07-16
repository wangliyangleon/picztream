#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/color/color.h"

using namespace pzt::core::color;
using pzt::core::decode::DecodedImage;

namespace {

DecodedImage make_image(const std::vector<std::array<std::uint8_t, 4>>& pixels, int width,
                         int height) {
  DecodedImage img;
  img.width = width;
  img.height = height;
  img.rgba.resize(pixels.size() * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    for (int c = 0; c < 4; ++c) img.rgba[i * 4 + static_cast<std::size_t>(c)] = pixels[i][c];
  }
  return img;
}

Lut3D make_identity_lut(int n) {
  Lut3D lut{n, std::vector<float>(static_cast<std::size_t>(n) * n * n * 3)};
  for (int r = 0; r < n; ++r) {
    for (int g = 0; g < n; ++g) {
      for (int b = 0; b < n; ++b) {
        std::size_t idx = (static_cast<std::size_t>(r) * n + g) * n + b;
        lut.data[idx * 3 + 0] = static_cast<float>(r) / (n - 1);
        lut.data[idx * 3 + 1] = static_cast<float>(g) / (n - 1);
        lut.data[idx * 3 + 2] = static_cast<float>(b) / (n - 1);
      }
    }
  }
  return lut;
}

// 每个网格点都映射到纯红色，用来验证 apply_lut 确实在改像素，不是原样
// 抄一遍。
Lut3D make_solid_red_lut(int n) {
  Lut3D lut{n, std::vector<float>(static_cast<std::size_t>(n) * n * n * 3, 0.f)};
  for (std::size_t i = 0; i < lut.data.size(); i += 3) lut.data[i] = 1.0f;
  return lut;
}

}  // namespace

TEST_CASE("apply_lut with an identity LUT leaves pixels unchanged (within rounding)") {
  auto img = make_image({{10, 200, 50, 255}, {0, 0, 0, 255}, {255, 255, 255, 255}}, 3, 1);
  auto original = img.rgba;
  apply_lut(img, make_identity_lut(17));
  for (std::size_t i = 0; i < img.rgba.size(); ++i) {
    CHECK(std::abs(static_cast<int>(img.rgba[i]) - static_cast<int>(original[i])) <= 1);
  }
}

TEST_CASE("apply_lut with a solid-color LUT maps every pixel to that color") {
  auto img = make_image({{10, 200, 50, 255}, {0, 0, 0, 255}}, 2, 1);
  apply_lut(img, make_solid_red_lut(17));
  CHECK(img.rgba[0] == 255);
  CHECK(img.rgba[1] == 0);
  CHECK(img.rgba[2] == 0);
  CHECK(img.rgba[4] == 255);
  CHECK(img.rgba[5] == 0);
  CHECK(img.rgba[6] == 0);
}

TEST_CASE("apply_lut with thread_count>1 matches the single-threaded result") {
  DecodedImage img1;
  img1.width = 4;
  img1.height = 20;  // 够多行才能真正切出多个 chunk
  img1.rgba.resize(static_cast<std::size_t>(4) * 20 * 4);
  for (int y = 0; y < 20; ++y) {
    for (int x = 0; x < 4; ++x) {
      std::size_t idx = (static_cast<std::size_t>(y) * 4 + x) * 4;
      img1.rgba[idx + 0] = static_cast<std::uint8_t>((x * 60 + y * 10) % 256);
      img1.rgba[idx + 1] = static_cast<std::uint8_t>((y * 13) % 256);
      img1.rgba[idx + 2] = static_cast<std::uint8_t>((x * 40) % 256);
      img1.rgba[idx + 3] = 255;
    }
  }
  DecodedImage img2 = img1;

  auto lut = make_identity_lut(17);
  // 用一个非平凡 LUT(不是纯色)更能暴露切分错误——warm 系数直接照抄
  // spikes/color_lut_probe/probe.cpp 的公式，具体数值不重要，只要求两次
  // 调用用同一个 lut。
  for (int r = 0; r < 17; ++r) {
    for (int g = 0; g < 17; ++g) {
      for (int b = 0; b < 17; ++b) {
        std::size_t idx = (static_cast<std::size_t>(r) * 17 + g) * 17 + b;
        float bf = static_cast<float>(b) / 16;
        lut.data[idx * 3 + 2] = std::clamp(bf - 0.08f * std::sin(bf * 3.14159f), 0.f, 1.f);
      }
    }
  }

  apply_lut(img1, lut, 1);
  apply_lut(img2, lut, 4);
  CHECK(img1.rgba == img2.rgba);
}

TEST_CASE("apply_adjustments boosts highlights in bright pixels and leaves dark pixels alone") {
  auto img = make_image({{200, 200, 200, 255}, {10, 10, 10, 255}}, 2, 1);
  apply_adjustments(img, /*highlights=*/20, /*shadows=*/0, 0, 0);
  CHECK(img.rgba[0] > 200);   // 亮像素被 highlights 正值推高
  CHECK(img.rgba[0] < 255);   // 但没有直接顶到最大值(这次输入不该触发裁剪)
  CHECK(std::abs(static_cast<int>(img.rgba[4]) - 10) <= 1);  // 暗像素 highlight_weight≈0，基本不变
}

TEST_CASE("apply_adjustments boosts shadows in dark pixels and leaves bright pixels alone") {
  auto img = make_image({{10, 10, 10, 255}, {200, 200, 200, 255}}, 2, 1);
  apply_adjustments(img, /*highlights=*/0, /*shadows=*/20, 0, 0);
  CHECK(img.rgba[0] > 10);
  CHECK(std::abs(static_cast<int>(img.rgba[4]) - 200) <= 1);
}

TEST_CASE("apply_adjustments applies white balance as a per-channel gain") {
  auto img = make_image({{100, 100, 100, 255}}, 1, 1);
  apply_adjustments(img, 0, 0, /*wb_shift_r=*/50, /*wb_shift_b=*/-50);
  CHECK(img.rgba[0] == 150);  // 100 * 1.5
  CHECK(img.rgba[1] == 100);  // 绿色通道不受白平衡影响
  CHECK(img.rgba[2] == 50);   // 100 * 0.5
}

TEST_CASE("apply_adjustments clamps at extreme parameter values without overflow") {
  auto img = make_image({{250, 250, 250, 255}}, 1, 1);
  apply_adjustments(img, /*highlights=*/1000, /*shadows=*/0, /*wb_shift_r=*/1000,
                     /*wb_shift_b=*/1000);
  CHECK(img.rgba[0] == 255);
  CHECK(img.rgba[1] == 255);
  CHECK(img.rgba[2] == 255);
}

TEST_CASE("apply_grain with amount=0 leaves pixels unchanged") {
  auto img = make_image({{100, 100, 100, 255}, {50, 50, 50, 255}}, 2, 1);
  auto original = img.rgba;
  apply_grain(img, 0.f);
  CHECK(img.rgba == original);
}

TEST_CASE("apply_grain with amount>0 perturbs at least some pixels") {
  DecodedImage img;
  img.width = 8;
  img.height = 8;
  img.rgba.assign(static_cast<std::size_t>(8) * 8 * 4, 128);
  for (std::size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;  // alpha
  auto original = img.rgba;
  apply_grain(img, 1.f);
  int changed = 0;
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    if (img.rgba[i] != original[i]) ++changed;
  }
  CHECK(changed > 0);
}

TEST_CASE("apply_grain is deterministic across repeated calls on the same input") {
  auto img1 = make_image({{100, 150, 200, 255}, {10, 20, 30, 255}}, 2, 1);
  auto img2 = img1;
  apply_grain(img1, 0.6f);
  apply_grain(img2, 0.6f);
  CHECK(img1.rgba == img2.rgba);
}

TEST_CASE("apply_grain clamps at extreme pixel values without overflow") {
  auto img = make_image({{255, 255, 255, 255}, {0, 0, 0, 255}}, 2, 1);
  apply_grain(img, 1.f);
  for (auto v : img.rgba) CHECK(v <= 255);  // 无符号溢出的话会绕回一个很小的数
}

TEST_CASE("apply_grain with thread_count>1 matches the single-threaded result") {
  DecodedImage img1;
  img1.width = 4;
  img1.height = 20;
  img1.rgba.assign(static_cast<std::size_t>(4) * 20 * 4, 0);
  for (std::size_t i = 0; i < img1.rgba.size(); ++i) {
    img1.rgba[i] = static_cast<std::uint8_t>(i % 200);
  }
  DecodedImage img2 = img1;
  apply_grain(img1, 0.7f, 1);
  apply_grain(img2, 0.7f, 4);
  CHECK(img1.rgba == img2.rgba);
}
