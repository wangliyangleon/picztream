#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "core/ai/score_worker.h"
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

// score_worker 的处理流程会真的调用 decode_preview_file——跟其它测试用
// touch() 写几个字节假装成 JPEG 不同，这里必须是一张能被真实解码的图
// 片，不然还没走到假 ScoreFn 就会在解码这一步失败。
void write_real_jpeg(const fs::path& p) {
  fs::create_directories(p.parent_path());
  decode::DecodedImage img;
  img.width = 4;
  img.height = 4;
  img.rgba.resize(4 * 4 * 4, 128);
  auto result = decode::encode_jpeg_file(img, p.string());
  REQUIRE(result.ok());
}

// 假 ScoreFn 立即返回，后台线程处理一个请求应该是毫秒级——用一个短间隔
// 的忙等循环，给个宽松的超时上限，避免真的卡住时无限等待。
bool wait_for_result(ScoreWorker& worker, std::uint64_t& generation) {
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
  Fixture fx("score_worker_dedup");
  auto fake_score = [](const decode::DecodedImage&, const std::string&,
                        Provider) -> Result<ScoreResult, ScoreError> {
    return Result<ScoreResult, ScoreError>::Ok(ScoreResult{50, "ok"});
  };
  ScoreWorker worker(fx.db_path, fake_score);

  CHECK(worker.request(fx.image_id, Provider::Claude, "") == true);
  CHECK(worker.request(fx.image_id, Provider::Claude, "") == false);
  CHECK(worker.request(fx.image_id + 1, Provider::Claude, "") == true);
}

TEST_CASE("a successful request writes all four columns, ai_score_prompt is the raw extra_guidance") {
  Fixture fx("score_worker_success");
  auto fake_score = [](const decode::DecodedImage&, const std::string&,
                        Provider) -> Result<ScoreResult, ScoreError> {
    return Result<ScoreResult, ScoreError>::Ok(ScoreResult{87, "Warm tones, slightly soft focus."});
  };
  ScoreWorker worker(fx.db_path, fake_score);

  CHECK(worker.request(fx.image_id, Provider::Gemini, "focus on the crop"));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  REQUIRE(info->ai_score.has_value());
  CHECK(*info->ai_score == 87);
  REQUIRE(info->ai_score_comment.has_value());
  CHECK(*info->ai_score_comment == "Warm tones, slightly soft focus.");
  REQUIRE(info->ai_score_prompt.has_value());
  CHECK(*info->ai_score_prompt == "focus on the crop");
  REQUIRE(info->ai_score_provider.has_value());
  CHECK(*info->ai_score_provider == "gemini");

  CHECK(!worker.has_pending());
  CHECK(worker.request(fx.image_id, Provider::Claude, ""));  // 完成后去重状态清除，可以再请求
}

TEST_CASE("a failed request leaves the four columns NULL") {
  Fixture fx("score_worker_failure");
  auto fake_score = [](const decode::DecodedImage&, const std::string&,
                        Provider) -> Result<ScoreResult, ScoreError> {
    return Result<ScoreResult, ScoreError>::Err(ScoreError::HttpError);
  };
  ScoreWorker worker(fx.db_path, fake_score);

  CHECK(worker.request(fx.image_id, Provider::Claude, ""));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  CHECK(!info->ai_score.has_value());
  CHECK(!info->ai_score_comment.has_value());
  CHECK(!info->ai_score_prompt.has_value());
  CHECK(!info->ai_score_provider.has_value());
  CHECK(!worker.has_pending());
}

TEST_CASE("a request for a nonexistent image completes without crashing or getting stuck") {
  auto db_path = fresh_db_path("score_worker_missing_image");
  Database::open_at(db_path);  // 建库但不建任何图片
  auto fake_score = [](const decode::DecodedImage&, const std::string&,
                        Provider) -> Result<ScoreResult, ScoreError> {
    return Result<ScoreResult, ScoreError>::Ok(ScoreResult{50, "ok"});
  };
  ScoreWorker worker(db_path, fake_score);

  CHECK(worker.request(999999, Provider::Claude, ""));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));
  CHECK(!worker.has_pending());
}

}  // namespace pzt::core::ai
