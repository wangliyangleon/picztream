#include "core/media/media.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>

#include "core/decode/decode.h"
#include "core/raw/raw.h"

namespace pzt::core::media {

namespace {

constexpr std::array<const char*, 2> kRawExtensions = {".dng", ".raf"};

}  // namespace

bool is_raw_path(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (const char* raw_ext : kRawExtensions) {
    if (ext == raw_ext) return true;
  }
  return false;
}

Result<decode::DecodedImage, decode::DecodeError> decode_preview_file(const std::string& path) {
  if (!is_raw_path(path)) {
    return decode::decode_jpeg_file(path);
  }
  auto bytes = raw::extract_embedded_jpeg_bytes(path);
  if (!bytes.ok()) {
    auto err = bytes.error() == raw::RawError::FileNotFound ? decode::DecodeError::FileNotFound
                                                            : decode::DecodeError::DecodeFailed;
    return Result<decode::DecodedImage, decode::DecodeError>::Err(err);
  }
  return decode::decode_jpeg_bytes(bytes.value());
}

std::string resolve_preview_path(const std::string& root_path, const std::string& file_path,
                                 const std::string& kind,
                                 const std::optional<std::string>& preview_cache_path) {
  if (kind == "raw" && preview_cache_path) return *preview_cache_path;
  return root_path + "/" + file_path;
}

}  // namespace pzt::core::media
