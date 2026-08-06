#include "core/ai/evaluation_worker.h"

#include <cstdio>
#include <utility>

#include "core/ai/evaluation_store.h"
#include "core/media/media.h"

namespace pzt::core::ai {

EvaluationWorker::EvaluationWorker(std::string db_path, EvaluationFn evaluation_fn)
    : db_path_(std::move(db_path)), evaluation_fn_(std::move(evaluation_fn)) {
  worker_ = std::jthread([this](std::stop_token st) { worker_loop(st); });
}

EvaluationWorker::~EvaluationWorker() = default;

bool EvaluationWorker::request(project::ImageId image_id, Provider provider,
                                const std::string& extra_guidance, bool auto_reject,
                                Language language, const LocalModelConfig& local_config) {
  std::unique_lock<std::mutex> lock(mu_);
  if (in_flight_.count(image_id)) return false;
  in_flight_.insert(image_id);
  queue_.push_back(
      PendingRequest{image_id, provider, extra_guidance, auto_reject, language, local_config});
  lock.unlock();
  cv_.notify_all();
  return true;
}

bool EvaluationWorker::has_pending() const {
  std::lock_guard<std::mutex> lock(mu_);
  return !in_flight_.empty();
}

bool EvaluationWorker::consume_new_result(std::uint64_t& last_seen_generation) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (generation_ == last_seen_generation) return false;
  last_seen_generation = generation_;
  return true;
}

EvaluationWorker::QueueStatus EvaluationWorker::queue_status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return QueueStatus{queue_.size(), in_flight_.size() > queue_.size(), failed_total_};
}

std::optional<EvaluationWorker::FailureReport> EvaluationWorker::take_failures() {
  std::lock_guard<std::mutex> lock(mu_);
  auto result = pending_failure_;
  pending_failure_.reset();
  return result;
}

void EvaluationWorker::worker_loop(std::stop_token stop) {
  while (true) {
    std::unique_lock<std::mutex> lock(mu_);
    bool have_work = cv_.wait(lock, stop, [&] { return !queue_.empty(); });
    if (!have_work) return;  // stop 已请求，且队列一直是空的
    // 跟 PrefetchCache::worker_loop 同样的理由：上面这个 wait 的谓词只看
    // 队列是否非空，stop 请求到达时队列里可能还排着好几个请求，谓词早就
    // 是 true 了。这里显式再查一次，stop 一旦被请求就不再捡新任务，最多
    // 完成"已经弹出、正在处理"的这一个。
    if (stop.stop_requested()) return;

    PendingRequest req = queue_.front();
    queue_.erase(queue_.begin());
    lock.unlock();

    auto failure = process_request(req);

    lock.lock();
    in_flight_.erase(req.image_id);
    // F-03：记下这次是不是失败的，供 take_failures() 取用——之前失败只
    // 打 stderr，不开 --debug 时用户完全看不到，见头文件里 FailureReport
    // 的说明。跟 generation_ 一样在这里(拿到锁之后)更新，process_request
    // 本身不碰这些受 mu_ 保护的状态。
    // T-23：累加而不是覆盖。图片与原因取最近这一次(它带着用户当下最该
    // 看到的具体原因)，次数从上一条累计上来——上一条还没被取走就说明用
    // 户还没看到过它，直接盖掉就是原来那个"800 张全失败只看到零星几条"
    // 的成因。
    if (failure) {
      int previous = pending_failure_ ? pending_failure_->count : 0;
      pending_failure_ = FailureReport{req.image_id, *failure, previous + 1};
      ++failed_total_;
    }
    ++generation_;
    lock.unlock();
    cv_.notify_all();
  }
}

// T-7：process_request 跑在后台 jthread 上，任何逃逸的异常都是
// std::terminate,整个 pzt 直接死掉，用户连"发生了什么"都看不到。这条线
// 程上会 throw 的地方不止一处：Database::open_at(库损坏、schema 版本比
// 程序新)、db::Stmt 的构造函数、project/tagging 里所有 DAO 风格的函数。
// 所以包的是整个函数体，不是单独某一行。
//
// 捕获点放在这个边界而不是 worker_loop：worker_loop 拿到返回值之后还要在
// mu_ 下做 in_flight_ 清理、generation_ 自增、notify_all，在这里返回一个
// 普通的失败值就能让那些簿记全部走原有的唯一一条路径，不用改签名。这跟
// F-17 当初把"落库失败"从 throw 改成返回 StorageFailed 是同一个手法。
std::optional<EvaluationError> EvaluationWorker::process_request(const PendingRequest& req) {
  try {
    return process_request_impl(req);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld aborted: %s\n",
                 static_cast<long long>(req.image_id), e.what());
    return EvaluationError::DatabaseUnavailable;
  } catch (...) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld aborted: unknown\n",
                 static_cast<long long>(req.image_id));
    return EvaluationError::DatabaseUnavailable;
  }
}

std::optional<EvaluationError> EvaluationWorker::process_request_impl(const PendingRequest& req) {
  db::Database db = db::Database::open_at(db_path_);

  auto info = project::get_image(db, req.image_id);
  if (!info) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld not found\n",
                 static_cast<long long>(req.image_id));
    return EvaluationError::ImageUnavailable;
  }

  auto project_summary = project::open_project(db, info->project_id);
  if (!project_summary.ok()) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld project_id=%lld not found\n",
                 static_cast<long long>(req.image_id), static_cast<long long>(info->project_id));
    return EvaluationError::ImageUnavailable;
  }

  std::string path = media::resolve_preview_path(project_summary.value().root_path, info->file_path,
                                                 info->kind, info->preview_cache_path);
  auto decoded = media::decode_preview_file(path);
  if (!decoded.ok()) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld decode failed path=%s\n",
                 static_cast<long long>(req.image_id), path.c_str());
    return EvaluationError::ImageUnavailable;
  }

  auto result =
      evaluation_fn_(decoded.value(), req.extra_guidance, req.provider, req.language, req.local_config);
  if (!result.ok()) {
    // 失败(网络错误、解析失败等)不写库，也不清空这张图之前成功评估过的
    // 记录——旧结果仍然是有效信息，一次失败的重新评估不该把之前成功的
    // 结果抹掉。见 docs/history/M3_Eng_Design.md"core/ai/evaluation_worker.h/.cpp"
    // 一节。
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld evaluation request failed\n",
                 static_cast<long long>(req.image_id));
    return result.error();
  }

  const auto& r = result.value();
  // 落库 + auto_reject 打标签整个委托给 store_evaluation：票 05 起 curate
  // 也要写这张表(它在一次 headless 调用内同步评估预选集，用不了这套异步
  // 队列)，result_json 的形状必须只有一个地方定义。req.auto_reject 是调用
  // 方提交请求时传进来的显式参数，process_request 本身不读 Settings。
  auto stored = store_evaluation(db, req.image_id, r, req.extra_guidance, req.provider,
                                  req.auto_reject);
  if (!stored.ok()) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld failed to save evaluation result\n",
                 static_cast<long long>(req.image_id));
    return stored.error();
  }

  std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld unusable=%d\n",
               static_cast<long long>(req.image_id), r.unusable ? 1 : 0);
  return std::nullopt;
}

}  // namespace pzt::core::ai
