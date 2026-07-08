#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/browse/prefetch.h"

using namespace pzt::core::browse;
using pzt::core::Result;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::DecodeError;

namespace {

std::vector<ImageRef> make_images(int n) {
  std::vector<ImageRef> images;
  for (int i = 0; i < n; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "%c.jpg", 'a' + i);
    images.push_back(ImageRef{static_cast<ImageId>(i + 1), name, name, "jpeg", std::nullopt});
  }
  return images;
}

// path 是 root_path + "/" + file_path 拼出来的绝对路径，用文件名 substring
// 匹配即可，不需要真实文件。widths 里没有的文件名会被当成解码失败
// (FileNotFound)，用来模拟"文件缺失/损坏"这一路径。
DecodeFn make_fake_decode(std::unordered_map<std::string, int> widths,
                          std::atomic<int>* call_count = nullptr) {
  return [widths = std::move(widths), call_count](
             const std::string& path) -> Result<DecodedImage, DecodeError> {
    if (call_count) call_count->fetch_add(1);
    for (const auto& [name, width] : widths) {
      if (path.find(name) != std::string::npos) {
        DecodedImage img;
        img.width = width;
        img.height = 1;
        img.rgba = {1, 2, 3, 4};
        return Result<DecodedImage, DecodeError>::Ok(img);
      }
    }
    return Result<DecodedImage, DecodeError>::Err(DecodeError::FileNotFound);
  };
}

}  // namespace

TEST_CASE("PrefetchCache decodes the window around current and get() returns matching image") {
  auto images = make_images(5);  // a..e, ids 1..5
  std::unordered_map<std::string, int> widths = {
      {"a.jpg", 10}, {"b.jpg", 20}, {"c.jpg", 30}, {"d.jpg", 40}, {"e.jpg", 50}};
  PrefetchCache cache("/root", /*window=*/1, make_fake_decode(widths));

  cache.set_current(images, images[2].id);  // current = c, window = b,c,d

  auto c = cache.get(images[2].id);
  REQUIRE(c.ok());
  CHECK(c.value().width == 30);

  auto b = cache.get(images[1].id);
  REQUIRE(b.ok());
  CHECK(b.value().width == 20);

  auto d = cache.get(images[3].id);
  REQUIRE(d.ok());
  CHECK(d.value().width == 40);
}

TEST_CASE("PrefetchCache passes the preview cache path for kind=raw images, not the raw file path") {
  // M2 回归测试:kind="raw" 且 preview_cache_path 有值时,decode_fn 应该拿
  // 到缓存文件的绝对路径(本身就是 JPEG,解码飞快),而不是 root_path +
  // file_path 拼出来的原始 .dng/.raf 路径(会退化成走内嵌预览提取兜底,
  // 或者更糟——如果 decode_fn 是真实的 decode_jpeg_file,会直接把原始
  // RAW 字节丢给 ImageIO,触发系统自带的 RAW 解码器,慢且有些机型(比如
  // 富士 X-Trans)解码效果很差,这正是真机测试中发现的 bug)。
  ImageId with_cache_id = 1;
  ImageId without_cache_id = 2;
  std::vector<ImageRef> images = {
      ImageRef{with_cache_id, "photo1.RAF", "photo1.RAF", "raw", std::string("/cache/1.jpg")},
      ImageRef{without_cache_id, "photo2.RAF", "photo2.RAF", "raw", std::nullopt},
  };

  std::vector<std::string> seen_paths;
  DecodeFn recording_decode = [&](const std::string& path) -> Result<DecodedImage, DecodeError> {
    seen_paths.push_back(path);
    DecodedImage img;
    img.width = 1;
    img.height = 1;
    img.rgba = {0, 0, 0, 0};
    return Result<DecodedImage, DecodeError>::Ok(img);
  };

  PrefetchCache cache("/root", /*window=*/1, recording_decode);
  cache.set_current(images, with_cache_id);
  REQUIRE(cache.get(with_cache_id).ok());
  REQUIRE(cache.get(without_cache_id).ok());

  bool used_cache_path = false;
  bool used_raw_file_path = false;
  for (const auto& p : seen_paths) {
    if (p == "/cache/1.jpg") used_cache_path = true;
    if (p == "/root/photo2.RAF") used_raw_file_path = true;
  }
  CHECK(used_cache_path);       // 有缓存时用缓存路径,不是 /root/photo1.RAF
  CHECK(used_raw_file_path);    // 没缓存时退化成原始文件路径
}

TEST_CASE("PrefetchCache reports NotInWindow for images outside the window") {
  auto images = make_images(5);
  std::unordered_map<std::string, int> widths = {
      {"a.jpg", 10}, {"b.jpg", 20}, {"c.jpg", 30}, {"d.jpg", 40}, {"e.jpg", 50}};
  PrefetchCache cache("/root", /*window=*/1, make_fake_decode(widths));

  cache.set_current(images, images[2].id);  // window = b,c,d

  auto a = cache.get(images[0].id);
  REQUIRE(!a.ok());
  CHECK(a.error() == FetchError::NotInWindow);

  auto e = cache.get(images[4].id);
  REQUIRE(!e.ok());
  CHECK(e.error() == FetchError::NotInWindow);
}

TEST_CASE("PrefetchCache propagates decode failures as DecodeFailed") {
  auto images = make_images(3);  // a,b,c
  // b.jpg 故意不在 widths 里，制造解码失败。
  std::unordered_map<std::string, int> widths = {{"a.jpg", 10}, {"c.jpg", 30}};
  PrefetchCache cache("/root", /*window=*/1, make_fake_decode(widths));

  cache.set_current(images, images[1].id);  // current = b

  auto b = cache.get(images[1].id);
  REQUIRE(!b.ok());
  CHECK(b.error() == FetchError::DecodeFailed);
}

TEST_CASE("PrefetchCache window wraps around when window exceeds list size") {
  auto images = make_images(3);  // a,b,c
  std::unordered_map<std::string, int> widths = {{"a.jpg", 10}, {"b.jpg", 20}, {"c.jpg", 30}};
  PrefetchCache cache("/root", /*window=*/5, make_fake_decode(widths));

  cache.set_current(images, images[0].id);  // 窗口环绕整圈，覆盖全部 3 张
  for (const auto& img : images) {
    auto r = cache.get(img.id);
    REQUIRE(r.ok());
  }
}

TEST_CASE("PrefetchCache clears the window when current_id is nullopt or not in the list") {
  auto images = make_images(3);
  std::unordered_map<std::string, int> widths = {{"a.jpg", 10}, {"b.jpg", 20}, {"c.jpg", 30}};
  PrefetchCache cache("/root", /*window=*/1, make_fake_decode(widths));

  cache.set_current(images, images[1].id);
  REQUIRE(cache.get(images[1].id).ok());

  cache.set_current(images, std::nullopt);
  auto after_nullopt = cache.get(images[1].id);
  REQUIRE(!after_nullopt.ok());
  CHECK(after_nullopt.error() == FetchError::NotInWindow);

  cache.set_current(images, images[1].id);
  REQUIRE(cache.get(images[1].id).ok());

  cache.set_current(images, images[1].id + 999);  // 不在列表里，等同 nullopt
  auto after_stale = cache.get(images[1].id);
  REQUIRE(!after_stale.ok());
  CHECK(after_stale.error() == FetchError::NotInWindow);
}

TEST_CASE("PrefetchCache re-decodes an entry after it's evicted and re-enters the window") {
  auto images = make_images(5);  // a..e, ids 1..5
  std::unordered_map<std::string, int> widths = {
      {"a.jpg", 10}, {"b.jpg", 20}, {"c.jpg", 30}, {"d.jpg", 40}, {"e.jpg", 50}};
  std::atomic<int> calls{0};
  PrefetchCache cache("/root", /*window=*/1, make_fake_decode(widths, &calls));

  cache.set_current(images, images[0].id);  // window = {a,b,e}
  REQUIRE(cache.get(images[0].id).ok());
  int after_first = calls.load();
  CHECK(after_first > 0);

  cache.set_current(images, images[3].id);  // window = {c,d,e}，a/b 被驱逐
  auto evicted = cache.get(images[0].id);
  REQUIRE(!evicted.ok());
  CHECK(evicted.error() == FetchError::NotInWindow);

  cache.set_current(images, images[0].id);  // a 重新进窗口，应该重新调度解码
  auto a = cache.get(images[0].id);
  REQUIRE(a.ok());
  CHECK(calls.load() > after_first);
}

TEST_CASE("PrefetchCache::get blocks until the background decode completes") {
  std::mutex gate_mu;
  std::condition_variable gate_cv;
  bool unblock = false;

  DecodeFn slow_decode = [&](const std::string&) -> Result<DecodedImage, DecodeError> {
    std::unique_lock<std::mutex> lock(gate_mu);
    gate_cv.wait(lock, [&] { return unblock; });
    DecodedImage img;
    img.width = 99;
    img.height = 1;
    img.rgba = {0, 0, 0, 0};
    return Result<DecodedImage, DecodeError>::Ok(img);
  };

  auto images = make_images(1);
  PrefetchCache cache("/root", /*window=*/0, slow_decode);
  cache.set_current(images, images[0].id);

  std::atomic<bool> got_result{false};
  std::thread getter([&] {
    auto r = cache.get(images[0].id);
    CHECK(r.ok());
    got_result = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(!got_result.load());  // 解码还没放行，get() 应该仍在阻塞等待

  {
    std::lock_guard<std::mutex> lock(gate_mu);
    unblock = true;
  }
  gate_cv.notify_all();
  getter.join();
  CHECK(got_result.load());
}

TEST_CASE("PrefetchCache destructor only waits out the in-flight decode, not the whole backlog") {
  constexpr auto kDecodeTime = std::chrono::milliseconds(50);
  auto slow_decode = [kDecodeTime](const std::string&) -> Result<DecodedImage, DecodeError> {
    std::this_thread::sleep_for(kDecodeTime);
    DecodedImage img;
    img.width = 1;
    img.height = 1;
    img.rgba = {0, 0, 0, 0};
    return Result<DecodedImage, DecodeError>::Ok(img);
  };

  // window=4 -> 最多 9 张排队,每张都要 50ms 解码,不修复的话析构等它们全部
  // 解码完要接近 450ms。
  auto images = make_images(9);
  auto cache = std::make_unique<PrefetchCache>("/root", /*window=*/4, slow_decode);
  cache->set_current(images, images[4].id);

  // 让 worker 真正开始解码第一张,此时队列里还应该排着好几张没解码的。
  std::this_thread::sleep_for(kDecodeTime / 2);

  auto t0 = std::chrono::steady_clock::now();
  cache.reset();  // 触发析构:request_stop + join
  double elapsed_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  // 修复前:析构要等剩下最多 8 张都解码完,接近 8*50ms=400ms;修复后只需要
  // 等"当前正在解码"这一张剩下的部分(约 25ms),不会再去捡队列里剩下的新
  // 任务。留够宽松的余量避免测试本身不稳定,但要严格小于"drain 剩余全
  // 部"的量级。
  CHECK(elapsed_ms < kDecodeTime.count() * 3);
}
