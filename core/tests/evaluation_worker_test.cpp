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
#include "core/tagging/tagging.h"

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

// W2026-07-21：eval 结果是"一段文字 assessment + unusable flag"。默认样本
// unusable=false(可用)，大多数测试复用它。
EvaluationResult make_evaluation_result() {
  return EvaluationResult{"balanced composition, warm color, sharp", false};
}

// unusable=true 的样本——auto_reject 的"打废片"用例用它。
EvaluationResult make_unusable_evaluation_result() {
  return EvaluationResult{"subject badly out of focus", true};
}

bool has_reject_tag(Database& db, ImageId id) {
  for (const auto& t : tagging::tags_for_image(db, id)) {
    if (t.name == tagging::kRejectTagName) return true;
  }
  return false;
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
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, "", false) == true);
  CHECK(worker.request(fx.image_id, Provider::Claude, "", false) == false);
  CHECK(worker.request(fx.image_id + 1, Provider::Claude, "", false) == true);
}

TEST_CASE("a successful request writes all fields, extra_guidance is the raw guidance text") {
  Fixture fx("evaluation_worker_success");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Gemini, "focus on the crop", false));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  REQUIRE(info->evaluation.has_value());
  const auto& eval = *info->evaluation;
  CHECK(eval.assessment == "balanced composition, warm color, sharp");
  CHECK(eval.unusable == false);
  CHECK(eval.extra_guidance == "focus on the crop");
  CHECK(eval.provider == "gemini");

  CHECK(!worker.has_pending());
  CHECK(worker.request(fx.image_id, Provider::Claude, "", false));  // 完成后去重状态清除，可以再请求
}

TEST_CASE("a failed request leaves no evaluation row") {
  Fixture fx("evaluation_worker_failure");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::HttpError);
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, "", false));

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
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::NetworkError);
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, "", false));

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
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, "", false));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  CHECK(!worker.take_last_failure().has_value());
}

// auto_reject 现在是 request() 的显式参数，不再从 Settings.auto_ai_reject
// 读取——process_request 不知道调用方是交互路径还是 agent，物理隔离见
// docs/M4_PRD.md P6。W2026-07-21：判据从 passes_gate 三项阈值改成模型直接
// 给的 unusable flag——unusable=true 且 auto_reject=true 时，落库之后自动
// 给这张图打上"废片"系统标签。
TEST_CASE("auto_reject tags an unusable evaluation with the reject tag when true") {
  Fixture fx("evaluation_worker_auto_reject_fail");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_unusable_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, "", /*auto_reject=*/true));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  CHECK(has_reject_tag(db, fx.image_id));
}

// 同样 auto_reject=true，但这次评估可用(unusable=false)——不该被打废片。
TEST_CASE("auto_reject does not tag a usable evaluation") {
  Fixture fx("evaluation_worker_auto_reject_pass");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());  // 可用
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, "", /*auto_reject=*/true));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  CHECK(!has_reject_tag(db, fx.image_id));
}

// 默认(auto_reject=false)不自动打标签，即便 unusable——这是现有行为的
// 回归防护，上面绝大多数其它测试用例都隐式依赖这一点。
TEST_CASE("auto_reject leaves unusable evaluations untagged when false") {
  Fixture fx("evaluation_worker_auto_reject_disabled");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_unusable_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  CHECK(worker.request(fx.image_id, Provider::Claude, "", /*auto_reject=*/false));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  CHECK(!has_reject_tag(db, fx.image_id));
}

// F-17：process_request 落库那一步以前不检查 sqlite3_step 的返回值——
// AI 已经给出结果，但写库失败(磁盘满/库损坏)时会静默发生，generation_
// 照样 +1 触发一次什么都没变的空重绘。这里用真实的只读文件权限强迫写
// 入失败(而不是伪造返回码)，验证这条路径现在会被 take_last_failure()
// 捕获成 StorageFailed，跟其它失败路径统一走 F-03 建的通道，不 throw
// (process_request 跑在后台 jthread 上，未捕获异常会 std::terminate)。
TEST_CASE("a DB write failure after a successful AI response is reported as StorageFailed") {
  Fixture fx("evaluation_worker_storage_failed");
  auto fake_evaluation = [](const decode::DecodedImage&, const std::string&,
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  fs::permissions(fx.db_path, fs::perms::owner_read, fs::perm_options::replace);
  auto restore_perms = [&] {
    fs::permissions(fx.db_path, fs::perms::owner_all, fs::perm_options::replace);
  };

  CHECK(worker.request(fx.image_id, Provider::Claude, "", false));

  std::uint64_t generation = 0;
  bool got_result = wait_for_result(worker, generation);
  restore_perms();  // 不管断言接下来会不会失败，先把权限还原掉，不影响其它测试
  REQUIRE(got_result);

  auto failure = worker.take_last_failure();
  REQUIRE(failure.has_value());
  CHECK(failure->image_id == fx.image_id);
  CHECK(failure->error == EvaluationError::StorageFailed);

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  CHECK(!info->evaluation.has_value());  // 写入真的失败了，没有半成品行落地
}

TEST_CASE("a failed re-evaluation does not clear a previously successful result") {
  Fixture fx("evaluation_worker_keep_old_on_failure");
  bool succeed = true;
  auto fake_evaluation = [&](const decode::DecodedImage&, const std::string&,
                              Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    if (succeed) return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
    return Result<EvaluationResult, EvaluationError>::Err(EvaluationError::NetworkError);
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  std::uint64_t generation = 0;
  CHECK(worker.request(fx.image_id, Provider::Claude, "", false));
  REQUIRE(wait_for_result(worker, generation));

  succeed = false;
  CHECK(worker.request(fx.image_id, Provider::Claude, "retry", false));
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  REQUIRE(info->evaluation.has_value());  // 上一次成功的结果还在，没被这次失败清掉
  CHECK(info->evaluation->assessment == "balanced composition, warm color, sharp");
}

TEST_CASE("re-evaluating an image overwrites the previous result") {
  Fixture fx("evaluation_worker_overwrite");
  std::string assessment = "first pass";
  auto fake_evaluation = [&](const decode::DecodedImage&, const std::string&,
                              Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(EvaluationResult{assessment, false});
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  std::uint64_t generation = 0;
  CHECK(worker.request(fx.image_id, Provider::Claude, "", false));
  REQUIRE(wait_for_result(worker, generation));

  assessment = "second pass";
  CHECK(worker.request(fx.image_id, Provider::Claude, "", false));
  REQUIRE(wait_for_result(worker, generation));

  auto db = Database::open_at(fx.db_path);
  auto info = get_image(db, fx.image_id);
  REQUIRE(info.has_value());
  REQUIRE(info->evaluation.has_value());
  CHECK(info->evaluation->assessment == "second pass");
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
                                  Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
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

  CHECK(worker.request(*id_a, Provider::Claude, "", false));
  {
    std::unique_lock<std::mutex> lock(block_mu);
    block_cv.wait(lock, [&] { return entered_processing; });
  }
  auto processing_alone = worker.queue_status();
  CHECK(processing_alone.queued == 0);
  CHECK(processing_alone.processing == true);

  CHECK(worker.request(*id_b, Provider::Claude, "", false));
  CHECK(worker.request(*id_c, Provider::Claude, "", false));
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
                             Provider, Language, const LocalModelConfig&) -> Result<EvaluationResult, EvaluationError> {
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(db_path, fake_evaluation);

  CHECK(worker.request(999999, Provider::Claude, "", false));

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

TEST_CASE("request threads LocalModelConfig through to evaluation_fn_") {
  Fixture fx("evaluation_worker_local_config");
  LocalModelConfig captured_config;
  auto fake_evaluation = [&](const decode::DecodedImage&, const std::string&, Provider, Language,
                              const LocalModelConfig& config) -> Result<EvaluationResult, EvaluationError> {
    captured_config = config;
    return Result<EvaluationResult, EvaluationError>::Ok(make_evaluation_result());
  };
  EvaluationWorker worker(fx.db_path, fake_evaluation);

  LocalModelConfig config{"http://example:9999", "custom-model"};
  CHECK(worker.request(fx.image_id, Provider::Local, "", false, Language::Chinese, config));

  std::uint64_t generation = 0;
  REQUIRE(wait_for_result(worker, generation));
  CHECK(captured_config.base_url == "http://example:9999");
  CHECK(captured_config.model == "custom-model");
}

}  // namespace pzt::core::ai
