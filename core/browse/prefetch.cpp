#include "core/browse/prefetch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_set>
#include <utility>

namespace pzt::core::browse {

namespace {

using clk = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

void log_decode(ImageId id, const std::string& path, double elapsed_ms,
                 const Result<decode::DecodedImage, decode::DecodeError>& decoded) {
  if (decoded.ok()) {
    std::fprintf(stderr, "[pzt prefetch] decode image_id=%lld path=%s %dx%d %.2fms\n",
                 static_cast<long long>(id), path.c_str(), decoded.value().width,
                 decoded.value().height, elapsed_ms);
  } else {
    std::fprintf(stderr, "[pzt prefetch] decode image_id=%lld path=%s FAILED %.2fms\n",
                 static_cast<long long>(id), path.c_str(), elapsed_ms);
  }
}

void log_get(ImageId id, const char* outcome, double wait_ms) {
  std::fprintf(stderr, "[pzt prefetch] get image_id=%lld %s wait_ms=%.2f\n",
               static_cast<long long>(id), outcome, wait_ms);
}

// current 优先，然后按距离由近到远交替向前/向后展开，直到覆盖 2*window+1
// 张或者(列表更短时)整个 images——跟 next_image/prev_image 的循环折返语义
// 一致。用 seen 去重，覆盖 images 很短、window 很大导致环绕重复的情形。
std::vector<ImageId> window_priority_order(const std::vector<ImageRef>& images, std::size_t idx,
                                            std::size_t window) {
  std::size_t n = images.size();
  std::vector<ImageId> order;
  std::unordered_set<ImageId> seen;
  auto try_add = [&](std::size_t index) {
    ImageId id = images[index % n].id;
    if (seen.insert(id).second) order.push_back(id);
  };

  try_add(idx);
  for (std::size_t d = 1; d <= window; ++d) {
    try_add(idx + d);
    try_add(idx + n - (d % n));
  }
  return order;
}

}  // namespace

PrefetchCache::PrefetchCache(std::string root_path, std::size_t window, DecodeFn decode_fn)
    : root_path_(std::move(root_path)), window_(window), decode_fn_(std::move(decode_fn)) {
  worker_ = std::jthread([this](std::stop_token st) { worker_loop(st); });
}

// jthread 成员在声明顺序里排最后，析构时按逆序先于 mu_/cache_ 等成员被销
// 毁——它的析构函数会自动 request_stop() 再 join()，保证 worker 线程在其
// 它成员真正被销毁之前已经彻底退出，不会有工作线程访问悬空引用。
PrefetchCache::~PrefetchCache() = default;

void PrefetchCache::set_current(const std::vector<ImageRef>& images,
                                 std::optional<ImageId> current_id) {
  std::unique_lock<std::mutex> lock(mu_);

  auto it = current_id ? std::find_if(images.begin(), images.end(),
                                       [&](const ImageRef& r) { return r.id == *current_id; })
                        : images.end();
  if (images.empty() || !current_id || it == images.end()) {
    cache_.clear();
    paths_.clear();
    pending_queue_.clear();
    return;
  }

  std::size_t idx = static_cast<std::size_t>(std::distance(images.begin(), it));
  std::vector<ImageId> wanted = window_priority_order(images, idx, window_);
  std::unordered_set<ImageId> wanted_set(wanted.begin(), wanted.end());

  for (auto cache_it = cache_.begin(); cache_it != cache_.end();) {
    if (!wanted_set.count(cache_it->first)) {
      paths_.erase(cache_it->first);
      cache_it = cache_.erase(cache_it);
    } else {
      ++cache_it;
    }
  }

  std::unordered_map<ImageId, const std::string*> path_by_id;
  for (const auto& ref : images) path_by_id[ref.id] = &ref.file_path;

  pending_queue_.clear();
  for (ImageId id : wanted) {
    auto cache_it = cache_.find(id);
    if (cache_it == cache_.end()) {
      cache_.emplace(id, Entry{State::Pending, {}});
      paths_[id] = root_path_ + "/" + *path_by_id.at(id);
      pending_queue_.push_back(id);
    } else if (cache_it->second.state == State::Pending) {
      pending_queue_.push_back(id);
    }
  }

  lock.unlock();
  cv_.notify_all();
}

Result<decode::DecodedImage, FetchError> PrefetchCache::get(ImageId id) {
  auto t0 = clk::now();
  std::unique_lock<std::mutex> lock(mu_);

  auto initial = cache_.find(id);
  if (initial == cache_.end()) {
    log_get(id, "not_in_window", 0.0);
    return Result<decode::DecodedImage, FetchError>::Err(FetchError::NotInWindow);
  }
  bool was_pending = initial->second.state == State::Pending;

  cv_.wait(lock, [&] {
    auto cur = cache_.find(id);
    return cur == cache_.end() || cur->second.state != State::Pending;
  });

  double elapsed = ms(clk::now() - t0).count();
  auto cur = cache_.find(id);
  if (cur == cache_.end()) {
    log_get(id, "not_in_window", elapsed);
    return Result<decode::DecodedImage, FetchError>::Err(FetchError::NotInWindow);
  }
  if (cur->second.state == State::Failed) {
    log_get(id, "decode_failed", elapsed);
    return Result<decode::DecodedImage, FetchError>::Err(FetchError::DecodeFailed);
  }
  log_get(id, was_pending ? "miss" : "hit", elapsed);
  return Result<decode::DecodedImage, FetchError>::Ok(cur->second.image);
}

void PrefetchCache::worker_loop(std::stop_token stop) {
  while (true) {
    std::unique_lock<std::mutex> lock(mu_);
    bool have_work = cv_.wait(lock, stop, [&] { return !pending_queue_.empty(); });
    if (!have_work) return;  // stop 已请求，且队列一直是空的
    // 上面这个 wait 的谓词只看队列是否非空——如果 stop 请求到达时队列里还
    // 排着好几张图，谓词早就是 true 了，不会因为 stop 被请求而提前返回。
    // 不额外检查的话，worker 会把队列剩下的每一张图都解码完才注意到
    // stop，退出的项目里全靠这个循环，之前真机测试量到过退出要卡 1-2
    // 秒。这里补一个显式检查：stop 一旦被请求，哪怕队列还有活，也不再捡
    // 新任务——最多完成"已经弹出、正在解码"的这一张（下面这个 continue
    // 判断之前的那几行还没执行到，不会有半解码状态）。
    if (stop.stop_requested()) return;

    ImageId id = pending_queue_.front();
    pending_queue_.erase(pending_queue_.begin());
    auto path_it = paths_.find(id);
    if (path_it == paths_.end() || !cache_.count(id)) continue;  // 导航期间被驱逐
    std::string path = path_it->second;
    lock.unlock();

    auto t0 = clk::now();
    auto decoded = decode_fn_(path);
    double elapsed = ms(clk::now() - t0).count();
    log_decode(id, path, elapsed, decoded);

    lock.lock();
    auto entry_it = cache_.find(id);
    if (entry_it == cache_.end()) continue;  // 解码期间被驱逐，丢弃结果
    if (decoded.ok()) {
      entry_it->second.state = State::Ready;
      entry_it->second.image = std::move(decoded.value());
    } else {
      entry_it->second.state = State::Failed;
    }
    lock.unlock();
    cv_.notify_all();
  }
}

}  // namespace pzt::core::browse
