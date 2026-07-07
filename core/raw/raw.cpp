#include "core/raw/raw.h"

#include <libraw/libraw.h>

#include <filesystem>

namespace pzt::core::raw {

namespace {

namespace fs = std::filesystem;

}  // namespace

// Increment 1 占位实现:只验证 CMake/pkg-config 接线到 LibRaw 是否真的
// 链接成功(构造 LibRaw、调用 open_file/recycle 都是真实符号,不是桩函数),
// 不实现 increment 2 要落地的"提取内嵌 JPEG 字节"这个真实逻辑——文件存在
// 且能被 LibRaw 打开时统一返回 DecodeFailed,提醒调用方这条路径还没实现,
// 不能把这个返回值当作真实的格式校验结果使用。
Result<std::vector<std::uint8_t>, RawError> extract_embedded_jpeg_bytes(const std::string& path) {
  if (!fs::exists(path)) {
    return Result<std::vector<std::uint8_t>, RawError>::Err(RawError::FileNotFound);
  }
  LibRaw proc;
  proc.open_file(path.c_str());
  proc.recycle();
  return Result<std::vector<std::uint8_t>, RawError>::Err(RawError::DecodeFailed);
}

// 同上，increment 1 占位实现。
Result<decode::DecodedImage, RawError> decode_full(const std::string& path) {
  if (!fs::exists(path)) {
    return Result<decode::DecodedImage, RawError>::Err(RawError::FileNotFound);
  }
  LibRaw proc;
  proc.open_file(path.c_str());
  proc.recycle();
  return Result<decode::DecodedImage, RawError>::Err(RawError::DecodeFailed);
}

}  // namespace pzt::core::raw
