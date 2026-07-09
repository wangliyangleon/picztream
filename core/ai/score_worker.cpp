#include "core/ai/score_worker.h"

#include <cstdio>
#include <utility>

#include "core/api.h"
#include "core/db/stmt.h"

namespace pzt::core::ai {

namespace {

std::string resolve_path(const std::string& root_path, const project::ImageInfo& info) {
  if (info.kind == "raw" && info.preview_cache_path) return *info.preview_cache_path;
  return root_path + "/" + info.file_path;
}

}  // namespace

ScoreWorker::ScoreWorker(std::string db_path, ScoreFn score_fn)
    : db_path_(std::move(db_path)), score_fn_(std::move(score_fn)) {
  worker_ = std::jthread([this](std::stop_token st) { worker_loop(st); });
}

ScoreWorker::~ScoreWorker() = default;

bool ScoreWorker::request(project::ImageId image_id, Provider provider,
                           const std::string& extra_guidance) {
  std::unique_lock<std::mutex> lock(mu_);
  if (in_flight_.count(image_id)) return false;
  in_flight_.insert(image_id);
  queue_.push_back(PendingRequest{image_id, provider, extra_guidance});
  lock.unlock();
  cv_.notify_all();
  return true;
}

bool ScoreWorker::has_pending() const {
  std::lock_guard<std::mutex> lock(mu_);
  return !in_flight_.empty();
}

bool ScoreWorker::consume_new_result(std::uint64_t& last_seen_generation) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (generation_ == last_seen_generation) return false;
  last_seen_generation = generation_;
  return true;
}

void ScoreWorker::worker_loop(std::stop_token stop) {
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

    process_request(req);

    lock.lock();
    in_flight_.erase(req.image_id);
    ++generation_;
    lock.unlock();
    cv_.notify_all();
  }
}

void ScoreWorker::process_request(const PendingRequest& req) {
  db::Database db = db::Database::open_at(db_path_);

  auto info = project::get_image(db, req.image_id);
  if (!info) {
    std::fprintf(stderr, "[pzt ai] score worker: image_id=%lld not found\n",
                 static_cast<long long>(req.image_id));
    return;
  }

  auto project_summary = project::open_project(db, info->project_id);
  if (!project_summary.ok()) {
    std::fprintf(stderr, "[pzt ai] score worker: image_id=%lld project_id=%lld not found\n",
                 static_cast<long long>(req.image_id), static_cast<long long>(info->project_id));
    return;
  }

  std::string path = resolve_path(project_summary.value().root_path, *info);
  auto decoded = decode_preview_file(path);
  if (!decoded.ok()) {
    std::fprintf(stderr, "[pzt ai] score worker: image_id=%lld decode failed path=%s\n",
                 static_cast<long long>(req.image_id), path.c_str());
    return;
  }

  auto score_result = score_fn_(decoded.value(), req.extra_guidance, req.provider);
  if (!score_result.ok()) {
    std::fprintf(stderr, "[pzt ai] score worker: image_id=%lld score request failed\n",
                 static_cast<long long>(req.image_id));
    return;
  }

  db::Stmt stmt(db.handle(),
                "UPDATE images SET ai_score = ?, ai_score_comment = ?, ai_score_prompt = ?, "
                "ai_score_provider = ? WHERE id = ?;");
  sqlite3_bind_int64(stmt.get(), 1, score_result.value().score);
  sqlite3_bind_text(stmt.get(), 2, score_result.value().comment.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, req.extra_guidance.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 4, to_string(req.provider), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.get(), 5, req.image_id);
  sqlite3_step(stmt.get());

  std::fprintf(stderr, "[pzt ai] score worker: image_id=%lld score=%d\n",
               static_cast<long long>(req.image_id), score_result.value().score);
}

}  // namespace pzt::core::ai
