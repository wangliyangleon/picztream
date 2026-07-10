#include <doctest.h>

#include <chrono>
#include <filesystem>
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
}

}  // namespace pzt::core::ai
