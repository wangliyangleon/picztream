#include "core/ai/evaluation_worker.h"

#include <cstdio>
#include <utility>

#include "core/db/stmt.h"
#include "core/media/media.h"
#include "core/tagging/tagging.h"

namespace pzt::core::ai {

EvaluationWorker::EvaluationWorker(std::string db_path, EvaluationFn evaluation_fn)
    : db_path_(std::move(db_path)), evaluation_fn_(std::move(evaluation_fn)) {
  worker_ = std::jthread([this](std::stop_token st) { worker_loop(st); });
}

EvaluationWorker::~EvaluationWorker() = default;

bool EvaluationWorker::request(project::ImageId image_id, Provider provider,
                                const std::string& extra_guidance, bool auto_reject,
                                const LocalModelConfig& local_config) {
  std::unique_lock<std::mutex> lock(mu_);
  if (in_flight_.count(image_id)) return false;
  in_flight_.insert(image_id);
  queue_.push_back(PendingRequest{image_id, provider, extra_guidance, auto_reject, local_config});
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

std::optional<EvaluationWorker::LastFailure> EvaluationWorker::take_last_failure() {
  std::lock_guard<std::mutex> lock(mu_);
  auto result = last_failure_;
  last_failure_.reset();
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
    // F-03：记下这次是不是失败的，供 take_last_failure() 取用——之前失
    // 败只打 stderr，不开 --debug 时用户完全看不到，见头文件里
    // LastFailure 的说明。跟 generation_ 一样在这里(拿到锁之后)更新，
    // process_request 本身不碰这些受 mu_ 保护的状态。
    if (failure) {
      last_failure_ = LastFailure{req.image_id, *failure};
    }
    ++generation_;
    lock.unlock();
    cv_.notify_all();
  }
}

std::optional<EvaluationError> EvaluationWorker::process_request(const PendingRequest& req) {
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

  auto result = evaluation_fn_(decoded.value(), req.extra_guidance, req.provider, req.local_config);
  if (!result.ok()) {
    // 失败(网络错误、解析失败等)不写库，也不清空这张图之前成功评估过的
    // 记录——旧结果仍然是有效信息，一次失败的重新评估不该把之前成功的
    // 结果抹掉。见 docs/M3_Eng_Design.md"core/ai/evaluation_worker.h/.cpp"
    // 一节。
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld evaluation request failed\n",
                 static_cast<long long>(req.image_id));
    return result.error();
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
  // F-17：以前不检查这一步——AI 已经给出结果，但落库失败(磁盘满、库损
  // 坏)时会静默发生，generation_ 照样 +1 触发一次什么都没变的空重绘，
  // 用户完全看不到发生了什么。这里不像 recipe.cpp 那些函数一样直接
  // throw：process_request 跑在后台 jthread 上，未捕获异常会
  // std::terminate，风险比"结果暂时没存上"这件事本身还大；改成走
  // F-03 刚建好的 last_failure_ 通道，跟其它失败路径统一。
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld failed to save evaluation result\n",
                 static_cast<long long>(req.image_id));
    return EvaluationError::StorageFailed;
  }

  // auto_reject：结果已经落库之后再判断达标与否，走 passes_gate 复用同
  // 一份判定，不在这里重算一遍三项阈值比较。只在不达标时打标签，不做
  // 反向摘除(见 core/settings/settings.h 里的说明)——这里用已经打开的
  // db 连接直接调 tagging:: 里的函数，不经过 core/api.h 门面(那边会各
  // 自开一条新连接，没必要)。req.auto_reject 是调用方（提交请求时）传
  // 进来的显式参数，process_request 本身不读 Settings。
  if (req.auto_reject) {
    EvaluationInfo eval_info{r.exposure,    r.exposure_fix,     r.composition,
                              r.composition_fix, r.focus,       r.comment,
                              req.extra_guidance, to_string(req.provider)};
    if (!passes_gate(eval_info)) {
      auto reject_tag_id = tagging::ensure_reject_tag(db, info->project_id);
      (void)tagging::add_tag(db, req.image_id, reject_tag_id);
    }
  }

  std::fprintf(stderr, "[pzt ai] evaluation worker: image_id=%lld exposure=%d composition=%d focus=%d\n",
               static_cast<long long>(req.image_id), r.exposure.score, r.composition.score,
               r.focus.score);
  return std::nullopt;
}

}  // namespace pzt::core::ai
