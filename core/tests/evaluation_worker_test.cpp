#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "core/ai/evaluation_worker.h"
#include "core/db/database.h"
#include "core/decode/decode.h"
#include "core/project/project.h"

namespace fs = std::filesystem;
using pzt::core::Result;
using pzt::core::db::Database;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::get_image;
using pzt::core::project::ImageId;

namespace pzt::core::ai {

namespace {

std::string fresh_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / (tag + ".db")).string();
  fs::remove(path);
  return path;
}

fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

// evaluation_worker 的处理流程会真的调用 decode_preview_file——跟其它测
// 试用 touch() 写几个字节假装成 JPEG 不同，这里必须是一张能被真实解码的
// 图片，不然还没走到假 EvaluationFn 就会在解码这一步失败。
void write_real_jpeg(const fs::path& p) {
  fs::create_directories(p.parent_path());
  decode::DecodedImage img;
  img.width = 4;
  img.height = 4;
  img.rgba.resize(4 * 4 * 4, 128);
  auto result = decode::encode_jpeg_file(img, p.string());
  REQUIRE(result.ok());
}

EvaluationResult make_evaluation_result() {
  return EvaluationResult{
      DimensionAssessment{7, "slightly underexposed"},
      ExposureFix{15.0},
      DimensionAssessment{4, "horizon is tilted"},
      CompositionFix{2.5, 0.0, 0.0, 0.0, 5.0},
      DimensionAssessment{9, "sharp"},
      "overall solid, mainly the tilted horizon",
  };
}

// 假 EvaluationFn 立即返回，后台线程处理一个请求应该是毫秒级——用一个短
// 间隔的忙等循环，给个宽松的超时上限，避免真的卡住时无限等待。
bool wait_for_result(EvaluationWorker& worker, std::uint64_t& generation) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (worker.consume_new_result(generation)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

struct Fixture {
  std::string db_path;
  ImageId image_id;

  explicit Fixture(const std::string& tag) {
    db_path = fresh_db_path(tag);
    auto photos = fresh_photo_dir(tag);
    write_real_jpeg(photos / "a.jpg");

    auto db = Database::open_at(db_path);
    auto created = create_project(db, "trip", photos.string());
    REQUIRE(created.ok());
    auto id = find_image_by_path(db, created.value(), "a.jpg");
    REQUIRE(id.has_value());
    image_id = *id;
  }
};

}  // namespace

TEST_CASE("request rejects a duplicate for the same image while one is in flight") {
  Fixture fx("evaluation_worker_dedup");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, "") == true);
  CHECK(worker.request(fx.image_id, Provider::Claude, "") == false);
  CHECK(worker.request(fx.image_id + 1, Provider::Claude, "") == true);
}

TEST_CASE("a successful request writes all fields, extra_guidance is the raw guidance text") {
  Fixture fx("evaluation_worker_success");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Gemini, "focus on the crop"));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  REQUIRE(info->evaluation.has_value());
  const auto& eval = *info->evaluation;
  CHECK(eval.exposure.score == 7);
  CHECK(eval.exposure.note == "slightly underexposed");
  REQUIRE(eval.exposure_fix.has_value());
  CHECK(eval.exposure_fix->adjust_percent == doctest::Approx(15.0));
  CHECK(eval.composition.score == 4);
  REQUIRE(eval.composition_fix.has_value());
  CHECK(eval.composition_fix->rotate_degrees == doctest::Approx(2.5));
  CHECK(eval.focus.score == 9);
  CHECK(eval.comment == "overall solid, mainly the tilted horizon");
  CHECK(eval.extra_guidance == "focus on the crop");
  CHECK(eval.provider == "gemini");

  CHECK(!worker.has_pending());
  CHECK(worker.request(fx.image_id, Provider::Claude, ""));  // 完成后去重状态清除，可以再请求
}

TEST_CASE("a failed request leaves no evaluation row") {
  Fixture fx("evaluation_worker_failure");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::HttpError);
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, ""));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  CHECK(!info->evaluation.has_value());
  CHECK(!worker.has_pending());
}

// F-03：失败原因原来只打 stderr，用户在 --debug 之外完全看不到。
// take_last_failure() 把它暴露出来，一次取走就清空——避免同一次失败被
// 反复报出来（跟 consume_new_result 的"消费一次"精神一致）。
TEST_CASE("a failed request is recorded in take_last_failure, consumed exactly once") {
  Fixture fx("evaluation_worker_last_failure");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::NetworkError);
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, ""));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto failure = worker.take_last_failure();
  REQUIRE(failure.has_value());
  CHECK(failure->image_id == fx.image_id);
  CHECK(failure->error == EvaluationError::NetworkError);

  CHECK(!worker.take_last_failure().has_value());  // 取走之后清空，不会重复报
}

TEST_CASE("a successful request leaves take_last_failure empty") {
  Fixture fx("evaluation_worker_last_failure_success");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, ""));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  CHECK(!worker.take_last_failure().has_value());
}

TEST_CASE("a failed re-evaluation does not clear a previously successful result") {
  Fixture fx("evaluation_worker_keep_old_on_failure");
  bool succeed = true;
  auto fake_evaluation = [&](const decode::DecodedImage&, const std::string&,
                              Provider) -> Result<EvaluationResult, EvaluationError> {
    if (succeed) return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::NetworkError);
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  std::uint64_t generation = 0;
  CHECK(worker.request(fx.image_id, Provider::Claude, ""));
  REQUIRE(wait_for_result(worker, generation));

  succeed = false;
  CHECK(worker.request(fx.image_id, Provider::Claude, "retry"));
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  REQUIRE(info->evaluation.has_value());  // 上一次成功的结果还在，没被这次失败清掉
  CHECK(info->evaluation->exposure.score == 7);
}

TEST_CASE("re-evaluating an image overwrites the previous result") {
  Fixture fx("evaluation_worker_overwrite");
  int score = 7;
  auto fake_evaluation = [&](const decode::DecodedImage&, const std::string&,
                              Provider) -> Result<EvaluationResult, EvaluationError> {
    auto result = make_evaluation_result();
    result.exposure.score = score;
    return Result<EvaluationResult, EvaluationError>::Ok(result);
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  std::uint64_t generation = 0;
  CHECK(worker.request(fx.image_id, Provider::Claude, ""));
  REQUIRE(wait_for_result(worker, generation));

  score = 2;
  CHECK(worker.request(fx.image_id, Provider::Claude, ""));
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  REQUIRE(info->evaluation.has_value());
  CHECK(info->evaluation->exposure.score == 2);
}

// M3:`/tasks` 用的聚合状态查询。单个 worker 线程一次只处理一个请求，
// "queued>0 但 processing 还是 false"这个状态在真实运行中只存在于请求
// 刚提交、worker 线程还没被调度醒来的极短窗口内，没法确定性地测——三个
// 有意义、能稳定复现的状态是:全空闲、只有一个在处理(没有排队积压)、一
// 个在处理+还有积压排队。用一个会阻塞在条件变量上的假 EvaluationFn 精
// 确控制"正在处理中"这个状态持续多久，见 docs/M3_Eng_Design.md 任务 6
// 的交叉检查说明。
TEST_CASE("queue_status reflects idle, processing-alone, and processing-with-backlog") {
  auto db_path = fresh_db_path("evaluation_worker_queue_status");
  auto photos = fresh_photo_dir("evaluation_worker_queue_status");
  write_real_jpeg(photos / "a.jpg");
  write_real_jpeg(photos / "b.jpg");
  write_real_jpeg(photos / "c.jpg");
  auto db = Database::open_at(db_path);
  auto created = create_project(db, "trip", photos.string());
  REQUIRE(created.ok());
  auto id_a = find_image_by_path(db, created.value(), "a.jpg");
  auto id_b = find_image_by_path(db, created.value(), "b.jpg");
  auto id_c = find_image_by_path(db, created.value(), "c.jpg");
  REQUIRE(id_a.has_value());
  REQUIRE(id_b.has_value());
  REQUIRE(id_c.has_value());

  std::mutex block_mu;
  std::condition_variable block_cv;
  bool release = false;
  bool entered_processing = false;

  auto blocking_evaluation = [&](const decode::DecodedImage&, const std::string&,
                                  Provider) -> Result<EvaluationResult, EvaluationError> {
    std::unique_lock<std::mutex> lock(block_mu);
    entered_processing = true;
    block_cv.notify_all();
    block_cv.wait(lock, [&] { return release; });
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(db_path, blocking_evaluation);

  auto idle = worker.queue_status();
  CHECK(idle.queued == 0);
  CHECK(idle.processing == false);

  CHECK(worker.request(*id_a, Provider::Claude, ""));
  {
    std::unique_lock<std::mutex> lock(block_mu);
    block_cv.wait(lock, [&] { return entered_processing; });
  }
  auto processing_alone = worker.queue_status();
  CHECK(processing_alone.queued == 0);
  CHECK(processing_alone.processing == true);

  CHECK(worker.request(*id_b, Provider::Claude, ""));
  CHECK(worker.request(*id_c, Provider::Claude, ""));
  auto with_backlog = worker.queue_status();
  CHECK(with_backlog.queued == 2);
  CHECK(with_backlog.processing == true);

  {
    std::lock_guard<std::mutex> lock(block_mu);
    release = true;
  }
  block_cv.notify_all();

  // 三个请求可能在两次 poll 之间就全部处理完(consume_new_result 只能告
  // 诉你"有没有新结果"，不是"这次具体完成了几个")，直接轮询目标状态本
  // 身，而不是假设每次 wait_for_result 恰好对应一个请求完成。
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  EvaluationWorker::QueueStatus done{1, true};
  while (std::chrono::steady_clock::now() < deadline) {
    done = worker.queue_status();
    if (done.queued == 0 && !done.processing) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(done.queued == 0);
  CHECK(done.processing == false);
}

TEST_CASE("a request for a nonexistent image completes without crashing or getting stuck") {
  auto db_path = fresh_db_path("evaluation_worker_missing_image");
  Database::open_at(db_path);  // 建库但不建任何图片
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(db_path, fake_evaluation);

  CHECK(worker.request(999999, Provider::Claude, ""));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));
  CHECK(!worker.has_pending());

  // F-03：请求真正发出去之前(图片记录都找不到)的失败,也要走同一条
  // last_failure_ 通道,不是只有"AI 请求本身失败"才算数。
  auto failure = worker.take_last_failure();
  REQUIRE(failure.has_value());
  CHECK(failure->image_id == 999999);
  CHECK(failure->error == EvaluationError::ImageUnavailable);
}

}  // namespace pzt::core::ai
