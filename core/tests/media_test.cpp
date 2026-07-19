#include <doctest.h>

#include <optional>
#include <string>

#include "core/media/media.h"

using pzt::core::media::is_raw_path;
using pzt::core::media::resolve_preview_path;

// F-16：core/media 把此前散落在 api.cpp/dedup.cpp/evaluation_worker.cpp/
// project.cpp 的 RAW 预览判断与路径解析收成单一来源。这里锁住纯函数行为；
// decode_preview_file 需要真实文件，由 evaluation_worker/dedup 的集成测试覆盖。

TEST_CASE("is_raw_path recognizes DNG/RAF case-insensitively") {
  CHECK(is_raw_path("/photos/a.dng"));
  CHECK(is_raw_path("/photos/a.raf"));
  CHECK(is_raw_path("/photos/A.DNG"));
  CHECK(is_raw_path("/photos/mixed.Raf"));
}

TEST_CASE("is_raw_path rejects non-RAW and extensionless paths") {
  CHECK_FALSE(is_raw_path("/photos/a.jpg"));
  CHECK_FALSE(is_raw_path("/photos/a.jpeg"));
  CHECK_FALSE(is_raw_path("/photos/a.png"));
  CHECK_FALSE(is_raw_path("/photos/noext"));
  CHECK_FALSE(is_raw_path(""));
}

TEST_CASE("resolve_preview_path uses cache only for raw kind with cache present") {
  const std::string root = "/root";
  const std::string file = "sub/img.dng";

  // raw + 有缓存 -> 缓存绝对路径
  CHECK(resolve_preview_path(root, file, "raw", std::string("/cache/img.jpg")) ==
        "/cache/img.jpg");

  // raw + 无缓存 -> root/file 兜底
  CHECK(resolve_preview_path(root, file, "raw", std::nullopt) == "/root/sub/img.dng");

  // jpeg 一律 root/file，即使误带了缓存字段
  CHECK(resolve_preview_path(root, "sub/img.jpg", "jpeg", std::nullopt) == "/root/sub/img.jpg");
  CHECK(resolve_preview_path(root, "sub/img.jpg", "jpeg", std::string("/cache/x.jpg")) ==
        "/root/sub/img.jpg");
}
