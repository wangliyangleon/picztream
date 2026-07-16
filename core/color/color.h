#pragma once

#include <vector>

#include "core/decode/decode.h"

// 纯像素运算，不碰数据库，不知道 recipe/preset/version 是什么概念——风
// 格上对齐 core/decode(字节/像素进，像素出，同步调用，调用方决定要不要
// 丢进线程)。见 docs/M1_Eng_Design.md "core/color/" 一节。
//
// 这里的三线性插值/白平衡算法照抄 spikes/color_lut_probe/probe.cpp 已经
// 用 Phase 0 spike 验证过性能的写法，这次是把它提升成生产代码，不是重新
// 设计。
namespace pzt::core::color {

// n*n*n*3 个 float32，行优先，值域 [0,1]，跟 recipe 模块序列化到
// recipes.base_lut 的格式一一对应。
struct Lut3D {
  int size = 0;
  std::vector<float> data;
};

// 原地处理 img。thread_count=1 时单线程同步(预览路径)，>1 时按行切分到
// 多个 jthread(导出全分辨率烘焙路径)，两条路径共用同一份循环体。
void apply_lut(decode::DecodedImage& img, const Lut3D& lut, unsigned thread_count = 1);

// 8 个 -100..100 的百分比旋钮,一次遍历里依次做完 白平衡 -> 整体明暗
// (highlights/shadows/blacks/whites 四项一起算出同一个 delta) -> 对比度
// -> 饱和度,顺序跟 detail::make_graded_lut(core/recipe/recipe.h)保持一
// 致,方便对照。core/color 依然不知道"version"是什么,这只是一捆颜色调
// 整参数,不是 core/recipe::VersionParams 本身——core/recipe::render 负
// 责做这两者之间的映射。
struct AdjustParams {
  double highlights = 0;
  double shadows = 0;
  double blacks = 0;
  double whites = 0;
  double wb_shift_r = 0;
  double wb_shift_b = 0;
  double contrast = 0;
  double saturation = 0;
};

// 具体公式（这次实现时定的工作假设，不是摄影级精确算法，先保证功能正
// 确、可测试）：
//   luminance = 0.299R + 0.587G + 0.114B（原始输入像素）
//   highlight_weight = clamp((luminance - 0.5) / 0.5, 0, 1)
//   shadow_weight    = clamp((0.5 - luminance) / 0.5, 0, 1)
//   white_weight     = clamp((luminance - 0.8) / 0.2, 0, 1)   // 比 highlight_weight 窄,只影响逼近纯白的像素
//   black_weight     = clamp((0.2 - luminance) / 0.2, 0, 1)   // 比 shadow_weight 窄,只影响逼近纯黑的像素
//   brightness_delta = highlights/100*highlight_weight + shadows/100*shadow_weight
//                     + whites/100*white_weight + blacks/100*black_weight
//   R1 = clamp(R * (1 + wb_shift_r/100) + brightness_delta, 0, 1)
//   G1 = clamp(G + brightness_delta, 0, 1)
//   B1 = clamp(B * (1 + wb_shift_b/100) + brightness_delta, 0, 1)
//   contrast_gain = 1 + contrast/100
//   R2 = clamp((R1-0.5)*contrast_gain + 0.5, 0, 1)   (G2/B2 同理)
//   luma2 = 0.299*R2 + 0.587*G2 + 0.114*B2
//   sat_gain = 1 + saturation/100
//   R3 = clamp(luma2 + (R2-luma2)*sat_gain, 0, 1)    (G3/B3 同理，即最终输出)
void apply_adjustments(decode::DecodedImage& img, const AdjustParams& params,
                        unsigned thread_count = 1);

// 纯位置哈希生成的单色噪声,不依赖图片内容/时间戳做种子——同一张图同一个
// recipe 重复渲染(预览滚动、prefetch 缓存复用)得到完全相同的颗粒图案,
// 不会看起来"闪烁"。amount 是 0..1 的强度系数,0 时调用方(core/recipe::
// render)会直接跳过整个调用,这里不重复判断。三通道加同一个偏移量(单
// 色噪声,更接近真实胶片颗粒对亮度而不是色相的影响),不区分亮部/暗部。
void apply_grain(decode::DecodedImage& img, float amount, unsigned thread_count = 1);

}  // namespace pzt::core::color
