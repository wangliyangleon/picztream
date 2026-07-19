#pragma once

#include <optional>
#include <string>

#include "core/decode/decode.h"

namespace pzt::core::media {

// RAW 预览相关的三段小逻辑的唯一归属：扩展名判断、按扩展名分发的预览解码、
// 以及"用缓存预览还是拼原始路径"的路径解析。此前这三段在 core/api.cpp、
// core/dedup/dedup.cpp、core/ai/evaluation_worker.cpp、core/project/project.cpp
// 各自复制了一份，"加新 RAW 格式时多处漂移"是真实陷阱（见 Fix-it F-16）；这里
// 收成单一来源，各调用方转调。

// M2：目前只认徕卡 DNG / 富士 RAF（docs/M2_PRD.md 明确的范围）。扩展名集合是
// 单一扩展点，给"以后加 CR2/CR3/NEF/ARW"留好，不需要在多处改判断逻辑。判断大
// 小写不敏感。
bool is_raw_path(const std::string& path);

// 预览"取水"函数——按扩展名分发：.jpg/.jpeg 走 decode::decode_jpeg_file；
// .dng/.raf 走 LibRaw 内嵌 JPEG 提取。只有 RAW 缓存缺失时才会传原始 .dng/.raf
// 路径走这里的兜底提取；正常情况下 browse/dedup 已按 kind/preview_cache_path
// 把要传的 path 决定好（见 resolve_preview_path）。
Result<decode::DecodedImage, decode::DecodeError> decode_preview_file(const std::string& path);

// kind=="raw" 且预览缓存已生成时，直接用缓存文件绝对路径；否则拼
// root_path + "/" + file_path。
std::string resolve_preview_path(const std::string& root_path, const std::string& file_path,
                                 const std::string& kind,
                                 const std::optional<std::string>& preview_cache_path);

}  // namespace pzt::core::media
