#include "core/ai/evaluation_worker.h"

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

EvaluationWorker::EvaluationWorker(std::string db_path, EvaluationFn evaluation_fn)
    : db_path_(std::move(db_path)), evaluation_fn_(std::move(evaluation_fn)) {
  worker_ = std::jthread([this](std::stop_token st) { worker_loop(st); });
}

EvaluationWorker::~EvaluationWorker() = default;

bool EvaluationWorker::request(project::ImageId image_id, Provider provider,
                                const std::string& extra_guidance) {
  std::unique_lock<std::mutex> lock(mu_);
  if (in_flight_.count(image_id)) return false;
  in_flight_.insert(image_id);
  queue_.push_back(PendingRequest{image_id, provider, extra_guidance});
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
  return QueueStatus{queue_.size(), in_flight_.size() > queue_.size()};
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

    process_request(req);

    lock.lock();
    in_flight_.erase(req.image_id);
    ++generation_;
    lock.unlock();
    cv_.notify_all();
  }
}

void EvaluationWorker::process_request(const PendingRequest& req) {
  db::Database db = db::Database::open_at(db_path_);

  auto info = project::get_image(db, req.image_id);
  if (!info) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld not found\n",
                 static_cast<long long>(req.image_id));
    return;
  }

  auto project_summary = project::open_project(db, info->project_id);
  if (!project_summary.ok()) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld project_id=%lld not found\n",
                 static_cast<long long>(req.image_id), static_cast<long long>(info->project_id));
    return;
  }

  std::string path = resolve_path(project_summary.value().root_path, *info);
  auto decoded = decode_preview_file(path);
  if (!decoded.ok()) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld decode failed path=%s\n",
                 static_cast<long long>(req.image_id), path.c_str());
    return;
  }

  auto result = evaluation_fn_(decoded.value(), req.extra_guidance, req.provider);
  if (!result.ok()) {
    // 失败(网络错误、解析失败等)不写库，也不清空这张图之前成功评估过的
    // 记录——旧结果仍然是有效信息，一次失败的重新评估不该把之前成功的
    // 结果抹掉。见 docs/M3_Eng_Design.md"core/ai/evaluation_worker.h/.cpp"
    // 一节。
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld evaluation request failed\n",
                 static_cast<long long>(req.image_id));
    return;
  }

  const auto& r = result.value();
  db::Stmt stmt(db.handle(),
                "INSERT INTO image_evaluations (image_id, exposure_score, exposure_note, "
                "exposure_fix_percent, composition_score, composition_note, "
                "composition_fix_rotate_degrees, composition_fix_crop_left_percent, "
                "composition_fix_crop_right_percent, composition_fix_crop_top_percent, "
                "composition_fix_crop_bottom_percent, focus_score, focus_note, comment, "
                "extra_guidance, provider) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                "ON CONFLICT(image_id) DO UPDATE SET "
                "exposure_score = excluded.exposure_score, "
                "exposure_note = excluded.exposure_note, "
                "exposure_fix_percent = excluded.exposure_fix_percent, "
                "composition_score = excluded.composition_score, "
                "composition_note = excluded.composition_note, "
                "composition_fix_rotate_degrees = excluded.composition_fix_rotate_degrees, "
                "composition_fix_crop_left_percent = excluded.composition_fix_crop_left_percent, "
                "composition_fix_crop_right_percent = excluded.composition_fix_crop_right_percent, "
                "composition_fix_crop_top_percent = excluded.composition_fix_crop_top_percent, "
                "composition_fix_crop_bottom_percent = excluded.composition_fix_crop_bottom_percent, "
                "focus_score = excluded.focus_score, "
                "focus_note = excluded.focus_note, "
                "comment = excluded.comment, "
                "extra_guidance = excluded.extra_guidance, "
                "provider = excluded.provider;");
  sqlite3_bind_int64(stmt.get(), 1, req.image_id);
  sqlite3_bind_int(stmt.get(), 2, r.exposure.score);
  sqlite3_bind_text(stmt.get(), 3, r.exposure.note.c_str(), -1, SQLITE_TRANSIENT);
  if (r.exposure_fix) {
    sqlite3_bind_double(stmt.get(), 4, r.exposure_fix->adjust_percent);
  } else {
    sqlite3_bind_null(stmt.get(), 4);
  }
  sqlite3_bind_int(stmt.get(), 5, r.composition.score);
  sqlite3_bind_text(stmt.get(), 6, r.composition.note.c_str(), -1, SQLITE_TRANSIENT);
  if (r.composition_fix) {
    sqlite3_bind_double(stmt.get(), 7, r.composition_fix->rotate_degrees);
    sqlite3_bind_double(stmt.get(), 8, r.composition_fix->crop_left_percent);
    sqlite3_bind_double(stmt.get(), 9, r.composition_fix->crop_right_percent);
    sqlite3_bind_double(stmt.get(), 10, r.composition_fix->crop_top_percent);
    sqlite3_bind_double(stmt.get(), 11, r.composition_fix->crop_bottom_percent);
  } else {
    sqlite3_bind_null(stmt.get(), 7);
    sqlite3_bind_null(stmt.get(), 8);
    sqlite3_bind_null(stmt.get(), 9);
    sqlite3_bind_null(stmt.get(), 10);
    sqlite3_bind_null(stmt.get(), 11);
  }
  sqlite3_bind_int(stmt.get(), 12, r.focus.score);
  sqlite3_bind_text(stmt.get(), 13, r.focus.note.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 14, r.comment.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 15, req.extra_guidance.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 16, to_string(req.provider), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt.get());

  std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld exposure=%d composition=%d focus=%d\n",
               static_cast<long long>(req.image_id), r.exposure.score, r.composition.score,
               r.focus.score);
}

}  // namespace pzt::core::ai
