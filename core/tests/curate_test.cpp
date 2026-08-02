#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "core/ai/evaluation_store.h"
#include "core/curate/curate.h"
#include "core/db/database.h"
#include "core/decode/decode.h"
#include "core/project/project.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::ai::Provider;
using pzt::core::db::Database;
using pzt::core::decode::DecodedImage;
using pzt::core::decode::encode_jpeg_file;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::tagging;
using namespace pzt::core::curate;

namespace {

std::string fresh_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / ("curate_" + tag + ".db")).string();
  fs::remove(path);
  fs::remove(path + "-wal");  // WAL 边车,理由见 db_test.cpp 的同名 helper
  fs::remove(path + "-shm");
  return path;
}

fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / ("curate_" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << "x";
}

// 跟 core/tests/dedup_test.cpp 的 make_fixture 是同一个模式：建一个带 N
// 张图片(a.jpg, b.jpg, ...)的项目，返回 project_id、按文件名排序的
// image_id 列表、项目根目录。
struct Fixture {
  Database db;
  ProjectId project_id;
  std::vector<ImageId> images;  // images[0]="a.jpg", images[1]="b.jpg", ...
  std::string root_path;
};

Fixture make_fixture(const std::string& tag, int image_count) {
  auto db = Database::open_at(fresh_db_path(tag));
  auto photos = fresh_photo_dir(tag);
  for (int i = 0; i < image_count; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    touch(photos / (name + ".jpg"));
  }
  auto created = create_project(db, "proj", photos.string());
  REQUIRE(created.ok());

  std::vector<ImageId> images;
  for (int i = 0; i < image_count; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    auto id = find_image_by_path(db, created.value(), name + ".jpg");
    REQUIRE(id.has_value());
    images.push_back(*id);
  }
  return Fixture{std::move(db), created.value(), std::move(images), photos.string()};
}

void set_captured_at(Database& db, ImageId id, std::int64_t value) {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.handle(), "UPDATE images SET captured_at = ? WHERE id = ?;", -1, &stmt,
                      nullptr);
  sqlite3_bind_int64(stmt, 1, value);
  sqlite3_bind_int64(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// 跟 dedup_test.cpp 的 write_solid_jpeg 同一个手法：两张字节完全相同的
// 纯色 JPEG 解码后逐像素相同，dHash 距离必为 0，不需要精确控制压缩细节
// 就能稳定制造"这是一组重复"的场景，供分簇相关用例覆盖真实解码路径。
bool write_solid_jpeg(const fs::path& path, int width, int height, unsigned char gray) {
  DecodedImage img;
  img.width = width;
  img.height = height;
  img.rgba.assign(static_cast<std::size_t>(width) * height * 4, gray);
  return encode_jpeg_file(img, path.string()).ok();
}

// 跟 dedup_test.cpp/compare_test.cpp 的 EnvVarGuard 是同一个写法，各自
// 文件独立一份是既有惯例。
struct EnvVarGuard {
  std::string name;
  std::optional<std::string> previous;

  EnvVarGuard(std::string n, const char* value) : name(std::move(n)) {
    const char* existing = std::getenv(name.c_str());
    if (existing) previous = existing;
    if (value) {
      setenv(name.c_str(), value, 1);
    } else {
      unsetenv(name.c_str());
    }
  }

  ~EnvVarGuard() {
    if (previous) {
      setenv(name.c_str(), previous->c_str(), 1);
    } else {
      unsetenv(name.c_str());
    }
  }
};

}  // namespace

// 票 04：预选集大小的纯函数。表驱动穷举边界，不需要数据库或网络(PRD
// 测试决策的第二条缝隙) - 这一块最容易写错的是钳制的三个方向(下界
// 1.5、上界候选集大小、ceil 的取整方向)，摘出来才能穷举。
TEST_CASE("preselect_size clamps by lower bound 1.5, candidate count, and ceils") {
  struct Case {
    int candidate_count;
    double multiplier;
    int count;
    int expected;
    const char* why;
  };
  const Case cases[] = {
      // 常规：M=2 默认值，池子够大 -> 2N
      {100, 2.0, 9, 18, "default M=2 gives 2N"},
      {100, 3.0, 4, 12, "M=3 gives 3N"},
      // 下界 1.5：M 小于 1.5 一律按 1.5 生效(M=1 时池子等于要选的数量,
      // 模型没有选择余地)
      {100, 1.0, 4, 6, "M=1 clamps up to 1.5"},
      {100, 0.0, 4, 6, "M=0 clamps up to 1.5"},
      {100, -5.0, 4, 6, "negative M clamps up to 1.5"},
      {100, 1.4999, 4, 6, "just below 1.5 clamps up"},
      {100, 1.5, 4, 6, "exactly 1.5 stays"},
      {100, 1.6, 4, 7, "above 1.5 is honored (ceil(6.4)=7)"},
      // ceil 向上取整：1.5·N 是奇数 N 时的半张要进一
      {100, 1.5, 3, 5, "ceil(4.5)=5"},
      {100, 1.5, 1, 2, "ceil(1.5)=2"},
      // 上界候选集大小：不设绝对条数上限，但不超过手上有的
      {5, 2.0, 4, 5, "capped by candidate count"},
      {4, 2.0, 4, 4, "candidate count == N: preselection is a no-op"},
      {6, 2.0, 4, 6, "between N and target: degenerates to no-op"},
      {8, 2.0, 4, 8, "target exactly equals candidate count"},
      {1000000000, 2.0, 1000000000, 1000000000, "no overflow at extreme N"},
      // 退化输入：候选集为空 / 非正的 count(curate 的契约保证 count>0，
      // 这里只保证不返回负数或崩)
      {0, 2.0, 4, 0, "empty candidate set"},
      {10, 2.0, 0, 0, "count=0 selects nothing"},
      {10, 2.0, -1, 0, "negative count selects nothing"},
      {-1, 2.0, 4, 0, "negative candidate count"},
  };
  for (const auto& c : cases) {
    CAPTURE(c.why);
    CHECK(detail::preselect_size(c.candidate_count, c.multiplier, c.count) == c.expected);
  }
}

// 票 06：模型返回的原始序号 -> 最终选择的纯函数(PRD 测试决策的第一条缝
// 隙)。决策十三的全部分支都在这里穷举：越界剔除、去重保序、≥N 取前 N、
// <N 整体退化。不碰网络也不碰数据库，所以这些边界不需要注入就能钉死。
TEST_CASE("resolve_selection cleans out-of-range and duplicate indices, then decides fallback") {
  struct Case {
    std::vector<int> raw;
    int pool_size;
    int count;
    std::vector<int> expected;  // 空 = 整体退化
    const char* why;
  };
  const Case cases[] = {
      // 正常路径：全合法，原样保序(顺序即交付顺序，不许排序)
      {{3, 1, 2}, 3, 3, {3, 1, 2}, "all valid, order preserved verbatim"},
      {{2, 1}, 5, 2, {2, 1}, "model order is not sorted"},
      // 越界剔除，其余按原序采纳-不是"任何不合法就整批扔"
      {{2, 5, 1}, 3, 2, {2, 1}, "out-of-range dropped, rest kept in order"},
      {{0, 1, 2}, 3, 2, {1, 2}, "0 is out of range (indices are 1-based)"},
      {{-1, 3, 2}, 3, 2, {3, 2}, "negative dropped"},
      {{4, 1, 2, 3}, 3, 3, {1, 2, 3}, "above pool_size dropped"},
      // 去重且保序：留第一次出现的那个位置
      {{2, 2, 1, 2}, 3, 2, {2, 1}, "duplicates dropped, first occurrence wins"},
      {{1, 2, 1, 3}, 3, 3, {1, 2, 3}, "duplicate in the middle does not shift the rest"},
      // 清洗后多于 count：取前 count
      {{3, 1, 2}, 3, 2, {3, 1}, "more than count: take the first count"},
      {{1, 9, 2, 9, 3}, 3, 1, {1}, "take the first one only"},
      // 清洗后不足 count：整体退化(不拿确定性结果补齐)
      {{1, 7}, 3, 2, {}, "only one valid left, needs 2 -> whole-batch fallback"},
      {{9, 9, 9}, 3, 1, {}, "everything out of range -> fallback"},
      {{2, 2, 2}, 3, 2, {}, "dedupe leaves too few -> fallback"},
      {{}, 3, 1, {}, "empty reply -> fallback"},
      // 边界与退化输入
      {{1}, 1, 1, {1}, "single-element pool"},
      {{1, 2}, 0, 1, {}, "empty pool: every index is out of range"},
      {{1, 2}, 3, 0, {}, "count=0 selects nothing"},
      {{1, 2}, 3, -1, {}, "negative count selects nothing"},
  };
  for (const auto& c : cases) {
    CAPTURE(c.why);
    CHECK(detail::resolve_selection(c.raw, c.pool_size, c.count) == c.expected);
  }
}

// W2026-07-21：curate 不再看 evaluation 记录，纯标签排除。原来"未评估就
// 排除""未达标(gate)就排除"两条用例整合成这一条——未评估的图照样进候选。
TEST_CASE("curate includes unevaluated images (no evaluation dependency)") {
  auto fx = make_fixture("no_eval_included", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);  // 时间差够大，各自成簇

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10);
  CHECK(result.returned == 2);
  // 簇数(2) < count(5)：每簇一代表，按 captured_at 降序。
  CHECK(result.selected == std::vector<ImageId>{fx.images[1], fx.images[0]});
}

TEST_CASE("curate excludes reject-tagged and duplicate-tagged images") {
  auto fx = make_fixture("excluded_tags", 3);
  for (int i = 0; i < 3; ++i) {
    set_captured_at(fx.db, fx.images[i], 1000 + i * 10000);
  }
  auto reject_tag = ensure_reject_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[1], reject_tag).ok());
  auto dup_tag = ensure_duplicate_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[2], dup_tag).ok());

  auto result = curate(fx.db, fx.project_id, std::nullopt, 5, 20, 10);
  CHECK(result.selected == std::vector<ImageId>{fx.images[0]});
}

TEST_CASE("curate insufficient candidates returns all of them, not an error") {
  auto fx = make_fixture("insufficient", 1);
  set_captured_at(fx.db, fx.images[0], 1000);

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10);
  CHECK(result.requested == 5);
  CHECK(result.returned == 1);
}

TEST_CASE("curate picks one representative per cluster when clusters >= N") {
  // 纯色图片内部梯度恒为 0，compute_dhash 对任意灰度值都会算出同一个哈
  // 希(0)——真正决定分不分进同一簇的是 cluster_by_time 这一步的时间窗，
  // 不是灰度值本身(跟 dedup_test.cpp 的 facade 测试用例是同一个道理)。
  // a/b 时间挨得近(同一簇,keep=b 时间更新那张)，c/d 分别离 a/b、离彼此
  // 都超过 20 秒时间窗(各自成一簇)。count=2，簇数=3 >= 2。
  auto fx = make_fixture("clusters_ge_n", 4);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "d.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);    // 跟 a 差 5 秒,同簇 -> keep=b
  set_captured_at(fx.db, fx.images[2], 100000);  // 跟 a/b、跟 d 都差超过 20 秒,独立簇
  set_captured_at(fx.db, fx.images[3], 200000);  // 跟其它三张都差超过 20 秒,独立簇

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  REQUIRE(result.returned == 2);
  // W2026-07-21：去分数后是纯时间多样性。代表 = {b(簇{a,b}的 keep), c, d}，
  // a 因为跟 b 同簇被排除在代表之外，不会入选——多样性保护的核心断言。
  // farthest-point：seed 取最新 d(200000)，再选离 d 时间最远的 b(1005)。
  CHECK(result.selected == std::vector<ImageId>{fx.images[3], fx.images[1]});
}

TEST_CASE("curate spreads selection across captured_at (time diversity)") {
  // 3 张纯色图，两两时间差都超过 20 秒时间窗，各自独立成簇(纯色图内部
  // 梯度恒为 0，真正拆开它们的是时间窗，不是灰度值，见上一条用例)。
  auto fx = make_fixture("tie_break_spread", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 0);
  set_captured_at(fx.db, fx.images[1], 100000);  // 离 a 差 100000
  set_captured_at(fx.db, fx.images[2], 100);     // 离 a 差 100

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  // W2026-07-21：纯时间多样性。seed 取最新 b(100000)；第二名额 a vs c，
  // 选离已选集(b)时间更远的 -> a(差 100000 > c 的 99900)。
  CHECK(result.selected == std::vector<ImageId>{fx.images[1], fx.images[0]});
}

// 票 01：选出来是哪几张由 farthest-point 决定，但交出去的顺序不该是它的
// 挑选顺序——那个算法每次挑离已选集最远的一张，产出的排列在时间上必然跳
// 跃，而这个列表顺序一路决定 Deliver 的发送次序。注意上面那条多样性用例
// 证明不了本条：它的挑选顺序恰好已经是时间降序。这里 4 张各自独立成簇，
// 挑选顺序是 d(3000) -> a(0) -> b(1000)，与时间序不同。
TEST_CASE("curate orders the result by captured_at, not by farthest-point pick order") {
  auto fx = make_fixture("chrono_order", 4);
  auto dir = fs::path(fx.root_path);
  for (int i = 0; i < 4; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    REQUIRE(write_solid_jpeg(dir / (name + ".jpg"), 16, 16, 120));
  }
  set_captured_at(fx.db, fx.images[0], 0);     // a
  set_captured_at(fx.db, fx.images[1], 1000);  // b
  set_captured_at(fx.db, fx.images[2], 2000);  // c
  set_captured_at(fx.db, fx.images[3], 3000);  // d

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);

  // 选中的集合仍由 farthest-point 决定：seed=d(最新)，再选离 d 最远的
  // a(差 3000)，再选 b/c 中离 {d,a} 最远的——两者 min 距离都是 1000，打
  // 平取 id 更小的 b。集合 = {a, b, d}，本票不改变这一点。
  // 按 id 排序后比集合(ImageId 不按文件名顺序分配，不能直接写字面量)。
  std::vector<ImageId> got = result.selected;
  std::vector<ImageId> want{fx.images[0], fx.images[1], fx.images[3]};
  std::sort(got.begin(), got.end());
  std::sort(want.begin(), want.end());
  CHECK(got == want);

  // 顺序改为按 captured_at 降序，跟簇数<N 那条分支既有的排序方向一致。
  CHECK(result.selected ==
        std::vector<ImageId>{fx.images[3], fx.images[1], fx.images[0]});
}

// 票 01：没有 captured_at 的照片必须有确定落位，且排序不引入不确定性。
//
// 这条用例必须自己会因为本票而改变结果，否则它什么也没证明。三张（两张
// 有时间 + 一张 NULL）是不够的：greedy_pick 在还有带时间的候选时结构上
// 永远不会挑走 NULL 那张，挑选顺序恰好等于时间序，改动前后完全一样。
//
// 这里用四张让两者分叉：a=0、b=1000、c=2000、d=NULL，count=4 全选。挑选
// 顺序是 c(seed 取最新) -> a(离 c 最远) -> b -> d(没时间，垫底)；按时间
// 降序则是 c -> b -> a -> d。a 与 b 换位，正是本票带来的差别。
TEST_CASE("curate places images without captured_at last, deterministically") {
  auto fx = make_fixture("chrono_null", 4);
  auto dir = fs::path(fx.root_path);
  for (int i = 0; i < 4; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    REQUIRE(write_solid_jpeg(dir / (name + ".jpg"), 16, 16, 120));
  }
  set_captured_at(fx.db, fx.images[0], 0);     // a
  set_captured_at(fx.db, fx.images[1], 1000);  // b
  set_captured_at(fx.db, fx.images[2], 2000);  // c
  // d 不设，保持 NULL

  auto first = curate(fx.db, fx.project_id, std::nullopt, /*count=*/4, 20, 10);
  REQUIRE(first.selected.size() == 4);
  CHECK(first.selected == std::vector<ImageId>{fx.images[2], fx.images[1],
                                                fx.images[0], fx.images[3]});
  CHECK(first.selected.back() == fx.images[3]);  // 无 captured_at 的排最后

  // 重复调用顺序完全一致，排序不引入不确定性。
  auto second = curate(fx.db, fx.project_id, std::nullopt, /*count=*/4, 20, 10);
  CHECK(second.selected == first.selected);
}

TEST_CASE("curate does not backfill from non-representative cluster members when clusters < N") {
  // 一簇 3 张近重复(a,b,c)，keep=c(captured_at 最新)，全部落进同一个候
  // 选池，count=2 > 簇数=1：只返回代表 c，不从簇内非代表回填凑数——回填
  // 会让结果里出现彼此近重复的图，违背多样性目的(见 curate.cpp 说明)。
  // returned(1) < requested(2)，不报错。
  auto fx = make_fixture("no_backfill", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  for (int i = 0; i < 3; ++i) set_captured_at(fx.db, fx.images[i], 1000 + i);  // c 最新 -> keep

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);
  CHECK(result.requested == 2);
  CHECK(result.returned == 1);
  CHECK(result.selected == std::vector<ImageId>{fx.images[2]});
}

TEST_CASE("curate returns one representative per cluster across multiple clusters when clusters < N") {
  // 两簇：{a,b}(keep=b,时间更新那张) 和 {c}(独立簇)，count=3 > 簇数=2。
  // 只返回两个代表，按 captured_at 降序 [c, b]，不回填 a。
  auto fx = make_fixture("multi_cluster_shortfall", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);    // 跟 a 同簇 -> keep=b
  set_captured_at(fx.db, fx.images[2], 100000);  // 独立簇

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);
  CHECK(result.returned == 2);
  CHECK(result.selected == std::vector<ImageId>{fx.images[2], fx.images[1]});
}

TEST_CASE("curate is deterministic across repeated calls with identical input") {
  auto fx = make_fixture("determinism", 5);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 5000);
  set_captured_at(fx.db, fx.images[2], 9000);
  set_captured_at(fx.db, fx.images[3], 13000);
  set_captured_at(fx.db, fx.images[4], 17000);

  auto first = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);
  auto second = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10);

  CHECK(first.requested == second.requested);
  CHECK(first.returned == second.returned);
  CHECK(first.selected == second.selected);
}

// W2026-07-21 目标二：ai_enabled=true 时走真实的 tournament::
// cluster_and_choose(不是注入假 compare_fn)。用 dedup_test.cpp 同一个技
// 巧——Provider::Claude 没设 ANTHROPIC_API_KEY 时确定性地 MissingApiKey、
// 不连真网络——让每个 size>=2 的簇退化成 keep_id，等价于 ai_enabled=false
// 的结果，藉此验证 ai_enabled 真的传到底、簇数<count 时两种模式返回同一
// 个确定性结果。
TEST_CASE("curate with ai_enabled=true and clusters<count returns the same winners as ai_enabled=false "
          "when the provider has no credentials (no real network call)") {
  EnvVarGuard key("ANTHROPIC_API_KEY", nullptr);
  auto fx = make_fixture("ai_fallback_shortfall", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 120));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);    // 跟 a 同簇，AI 失败退化选更新的 b
  set_captured_at(fx.db, fx.images[2], 100000);  // 独立单例，不发起任何 AI 调用

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10,
                        /*preselect_multiplier=*/2.0, /*ai_enabled=*/true, Provider::Claude);
  CHECK(result.returned == 2);
  CHECK(result.selected == std::vector<ImageId>{fx.images[2], fx.images[1]});  // 同非 AI 版本的结果
  CHECK(result.ai_fallback_count == 1);  // 只有 {a,b} 这一簇尝试过 AI 并退化，单例 c 不算
}

// 簇数(4 个 winner:两个 size>=2 簇的 keep_id + 两个单例) >= count(3)，验
// 证随机采样这一步：结果大小正确、是 winner 集合的子集、无重复——不断
// 言具体挑中哪几个(随机，PRD 已拍板接受不可复现)。同样用 Claude 无 key
// 的确定性退化，让 winner 集合本身可预测。
TEST_CASE("curate with ai_enabled=true and clusters>=count samples a correctly-sized subset of winners") {
  EnvVarGuard key("ANTHROPIC_API_KEY", nullptr);
  auto fx = make_fixture("ai_sample_subset", 6);
  auto dir = fs::path(fx.root_path);
  for (char name : {'a', 'b', 'c', 'd', 'e', 'f'}) {
    REQUIRE(write_solid_jpeg(dir / (std::string(1, name) + ".jpg"), 16, 16, 120));
  }
  set_captured_at(fx.db, fx.images[0], 1000);      // a: 簇1 跟 b
  set_captured_at(fx.db, fx.images[1], 1010);      // b: 簇1，更新 -> AI 失败时的 keep_id
  set_captured_at(fx.db, fx.images[2], 100000);    // c: 簇2 跟 d
  set_captured_at(fx.db, fx.images[3], 100010);    // d: 簇2，更新 -> AI 失败时的 keep_id
  set_captured_at(fx.db, fx.images[4], 200000);    // e: 独立单例
  set_captured_at(fx.db, fx.images[5], 300000);    // f: 独立单例

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10,
                        /*preselect_multiplier=*/2.0, /*ai_enabled=*/true, Provider::Claude);
  REQUIRE(result.returned == 3);
  CHECK(result.ai_fallback_count == 2);  // {a,b}、{c,d} 两簇都尝试过 AI 并退化，e/f 单例不算

  std::vector<ImageId> winner_pool{fx.images[1], fx.images[3], fx.images[4], fx.images[5]};
  for (auto id : result.selected) {
    CHECK(std::find(winner_pool.begin(), winner_pool.end(), id) != winner_pool.end());
  }
  std::vector<ImageId> sorted_selected = result.selected;
  std::sort(sorted_selected.begin(), sorted_selected.end());
  CHECK(std::adjacent_find(sorted_selected.begin(), sorted_selected.end()) == sorted_selected.end());
}

// 票 04：候选集在选择之前先按时间多样性裁成预选集，AI 开的那条路也走这
// 一刀。6 个单例簇(全是 size=1，不发起任何 AI 比较)，count=2、M=1.5 =>
// 预选集 3 张。farthest-point 是确定性的：seed 取最新 f(500000)，再取离
// 它最远的 a(0)，第三名额 c 与 d 的最小距离打平(都是 200000)、按 id 小
// 者胜出 -> c。随机采样只在这 3 张里发生，b/d/e 永远不会入选 - 不裁剪的
// 话它们都在池子里，重复跑必然会撞上。
TEST_CASE("curate clamps the candidate set to the preselection before the AI path samples") {
  EnvVarGuard key("ANTHROPIC_API_KEY", nullptr);
  auto fx = make_fixture("preselect_ai", 6);
  for (int i = 0; i < 6; ++i) set_captured_at(fx.db, fx.images[i], i * 100000);

  std::vector<ImageId> preselected{fx.images[5], fx.images[0], fx.images[2]};
  for (int run = 0; run < 20; ++run) {
    auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10,
                          /*preselect_multiplier=*/1.5, /*ai_enabled=*/true, Provider::Claude);
    REQUIRE(result.returned == 2);
    CHECK(result.ai_fallback_count == 0);  // 全是单例簇，没有比较可失败
    for (auto id : result.selected) {
      CHECK(std::find(preselected.begin(), preselected.end(), id) != preselected.end());
    }
  }
}

// 票 04：AI 关的那条路同样经过裁剪，且输出一字不变。farthest-point 是增
// 量贪心，"先挑 K 张再从这 K 张里挑 N 张"与"直接挑 N 张"选出同一个集合
// (每一步的 argmax 都落在 K 里)，而交付顺序自票 01 起一律由
// by_captured_at_desc 决定、与挑选顺序无关，两头都不受裁剪影响，所以裁
// 剪在这条路上外部不可观测 - 这条用例守的就是这个不变量，而不是某个新
// 行为。
TEST_CASE("curate preselection leaves the non-AI selection unchanged") {
  auto fx = make_fixture("preselect_non_ai", 6);
  for (int i = 0; i < 6; ++i) set_captured_at(fx.db, fx.images[i], i * 100000);

  auto clamped = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10,
                         /*preselect_multiplier=*/1.5);
  CHECK(clamped.selected == std::vector<ImageId>{fx.images[5], fx.images[0]});

  // M 大到裁剪退化成空操作时结果一样。
  auto unclamped = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10,
                           /*preselect_multiplier=*/100.0);
  CHECK(unclamped.selected == clamped.selected);
}

// 票 04：候选集小于 N 时裁剪不参与，沿用既有行为(全部返回、captured_at
// 降序的确定性排序)。M 取一个会把预选集算成 2 张的值也不影响。
TEST_CASE("curate skips preselection when the candidate set is smaller than N") {
  auto fx = make_fixture("preselect_shortfall", 2);
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 100000);

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10,
                        /*preselect_multiplier=*/1.5);
  CHECK(result.requested == 5);
  CHECK(result.returned == 2);
  CHECK(result.selected == std::vector<ImageId>{fx.images[1], fx.images[0]});
}

// T-8 A.2：进度回调。改造前 core/curate 连钩子都没有——dedup 是"有钩子
// 但 cmd_dedup 传 nullptr"，curate 是"签名里根本没这几个参数"。根因是
// 四个回调（on_progress/on_ai_gate/on_ai_progress/on_cancel）全部由 TUI
// 的 /dedup 驱动加进来，而 curate 没有 TUI 入口，从来没人走在那条路上。
// 只补有消费者的两个，理由与不补的那四项见 PRD 决策七。
TEST_CASE("curate reports local clustering progress") {
  auto fx = make_fixture("cluster_progress", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 200));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);
  set_captured_at(fx.db, fx.images[2], 100000);

  std::vector<std::pair<int, int>> seen;
  curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10, /*preselect_multiplier=*/2.0,
         /*ai_enabled=*/false, Provider::Local, pzt::core::ai::LocalModelConfig{},
         /*selection_brief=*/"",
         [&](int done, int total) { seen.emplace_back(done, total); });

  REQUIRE(!seen.empty());
  // 单调递增、最后一次 done==total：调用方拿它当进度条的刻度，回退或者
  // 停在半路都会让用户以为卡住了。
  for (std::size_t i = 1; i < seen.size(); ++i) CHECK(seen[i].first > seen[i - 1].first);
  CHECK(seen.back().first == seen.back().second);
}

TEST_CASE("curate reports AI comparison progress") {
  // 同上面几个 AI 用例的技巧：Provider::Claude 没设 key 时确定性地
  // MissingApiKey，不连真网络。进度是在发起比较之前报的，所以比较本身
  // 失败不影响这里要验的东西。
  EnvVarGuard key("ANTHROPIC_API_KEY", nullptr);
  auto fx = make_fixture("ai_progress", 3);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "c.jpg", 16, 16, 200));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1005);
  set_captured_at(fx.db, fx.images[2], 100000);

  std::vector<int> compares;
  curate(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10, /*preselect_multiplier=*/2.0,
         /*ai_enabled=*/true, Provider::Claude, pzt::core::ai::LocalModelConfig{},
         /*selection_brief=*/"", nullptr,
         [&](const pzt::core::dedup::AiProgress& p) { compares.push_back(p.comparison_done); });

  REQUIRE(!compares.empty());
  for (std::size_t i = 1; i < compares.size(); ++i) CHECK(compares[i] > compares[i - 1]);
}

TEST_CASE("curate with no progress callbacks behaves exactly as before") {
  // 默认 nullptr 保证现有调用点零改动，跟 W2026-07-21 给 dedup 加这些
  // 参数时同一个约定。
  auto fx = make_fixture("no_progress_cb", 2);
  auto dir = fs::path(fx.root_path);
  REQUIRE(write_solid_jpeg(dir / "a.jpg", 16, 16, 120));
  REQUIRE(write_solid_jpeg(dir / "b.jpg", 16, 16, 200));
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 100000);

  auto result = curate(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10);

  CHECK(result.returned == 2);
}

// ---------------------------------------------------------------------------
// 票 05：curate 内部评估预选集 - 闸门 + 进度 + ai_declined/cancelled
//
// 这一组用例全部走 detail::curate_impl 注入假 evaluate_fn。理由见
// curate.h 上 EvaluateFn 的说明：本票要覆盖的行为(闸门报的张数跟真实发
// 起的次数对不对得上、缓存跳过、进度计数、取消)只存在于成功路径上，而
// 这个文件既有的 AI 用例一律是"让调用必然失败、只验证退化"。
// ---------------------------------------------------------------------------

namespace {

// 记账用的假评估：记下被评估过的 id，按需汇报失败。真的往库里写一条评估
// 记录，好让"已评估过的跳过"这条能被下一次调用观察到。
struct FakeEvaluator {
  std::vector<ImageId> evaluated;
  bool succeed = true;

  bool operator()(Database& db, ImageId id) {
    evaluated.push_back(id);
    if (!succeed) return false;
    pzt::core::ai::EvaluationResult r{"锐利、构图均衡", false, "一只猫趴在窗台上"};
    return pzt::core::ai::store_evaluation(db, id, r, "", Provider::Local, /*auto_reject=*/false)
        .ok();
  }
};

bool has_evaluation(Database& db, ImageId id) {
  auto info = pzt::core::project::get_image(db, id);
  return info && info->evaluation.has_value();
}

// 全是单例簇的 fixture：图片时间上互相远离，一个 size>=2 的簇都没有，因
// 此一次比较都不会发起。用它把"评估"这条路径跟锦标赛隔离开单独观察。
Fixture make_singletons(const std::string& tag, int n) {
  auto fx = make_fixture(tag, n);
  for (int i = 0; i < n; ++i) set_captured_at(fx.db, fx.images[i], i * 100000);
  return fx;
}

}  // namespace

TEST_CASE("curate --ai evaluates exactly the preselection, not the whole library") {
  // 12 个单例簇、count=2、M=2 => 预选集 4 张。评估次数跟着预选集走，跟
  // 图库大小(12)无关 - 这是 PRD 决策十"由构造保证有界"的可观测形式。
  auto fx = make_singletons("eval_preselection", 12);
  FakeEvaluator eval;

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10,
                                     /*preselect_multiplier=*/2.0, /*ai_enabled=*/true,
                                     Provider::Local, pzt::core::ai::LocalModelConfig{},
                                     std::ref(eval));

  CHECK(result.returned == 2);
  CHECK(eval.evaluated.size() == 4);

  // 评估的必须正好是预选集本身，而不是随便 4 张。不把那 4 个 id 写死：
  // farthest-point 打平时按 image id 兜底，而 id 是按目录扫描顺序分配
  // 的、不保证跟文件名同序，写死等于把测试钉在文件系统的返回顺序上。
  // 改成断言一个不变量-预选集就是"关 AI 时挑 k 张"的结果，两者走的是
  // 同一个 take_farthest_points(票 04 的裁剪与最终选片共用同一个循环)。
  auto deterministic = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/4, 20, 10,
                                            2.0, /*ai_enabled=*/false, Provider::Local,
                                            pzt::core::ai::LocalModelConfig{}, std::ref(eval));
  std::vector<ImageId> expected = deterministic.selected;
  std::vector<ImageId> got = eval.evaluated;  // 上面那次关 AI 的调用不评估，不会增长
  std::sort(expected.begin(), expected.end());
  std::sort(got.begin(), got.end());
  CHECK(got == expected);
}

TEST_CASE("curate --ai skips photos that already have an evaluation") {
  auto fx = make_singletons("eval_cache_skip", 6);
  FakeEvaluator first;
  detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                       /*ai_enabled=*/true, Provider::Local, pzt::core::ai::LocalModelConfig{},
                       std::ref(first));
  REQUIRE(first.evaluated.size() == 4);

  // 同一批照片再跑一次：预选集是确定性的，四张全都已经有评估记录了，一
  // 次请求都不该再发出去(PRD 决策七：有记录就跳过，不做字段完整性检查)。
  FakeEvaluator second;
  detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                       /*ai_enabled=*/true, Provider::Local, pzt::core::ai::LocalModelConfig{},
                       std::ref(second));
  CHECK(second.evaluated.empty());
}

TEST_CASE("curate --ai gate is consulted before any evaluation and sees the exact counts") {
  auto fx = make_singletons("eval_gate_counts", 12);
  FakeEvaluator eval;

  int seen_comparisons = -1;
  int seen_evaluations = -1;
  auto result = detail::curate_impl(
      fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0, /*ai_enabled=*/true,
      Provider::Local, pzt::core::ai::LocalModelConfig{}, std::ref(eval), /*select_fn=*/nullptr,
      /*on_progress=*/nullptr,
      /*on_ai_progress=*/nullptr, [&](int comparisons, int evaluations) {
        seen_comparisons = comparisons;
        seen_evaluations = evaluations;
        // 闸门被问到的这一刻，一张都还没评估过。
        CHECK(eval.evaluated.empty());
        return true;
      });

  CHECK(result.returned == 2);
  CHECK(seen_comparisons == 0);  // 全是单例簇，没有任何比较
  CHECK(seen_evaluations == 4);
  // 闸门报的张数跟真实发起的次数对得上(冷缓存下是相等，热缓存下只会更少)。
  CHECK(static_cast<int>(eval.evaluated.size()) == seen_evaluations);
}

TEST_CASE("curate --ai gate is still consulted when there is nothing to compare") {
  // tournament 的闸门只在存在 size>=2 的簇时才问(没有比较就没有开销)，但
  // curate 即使一次比较都不发也仍然要评估，开销不为零。这条守的就是那个
  // 缺口 - 少了它，全是单例簇的项目会绕过闸门直接开始花钱。
  auto fx = make_singletons("eval_gate_no_groups", 6);
  FakeEvaluator eval;

  bool asked = false;
  auto result = detail::curate_impl(
      fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0, /*ai_enabled=*/true,
      Provider::Local, pzt::core::ai::LocalModelConfig{}, std::ref(eval), nullptr, nullptr, nullptr,
      [&](int, int) {
        asked = true;
        return false;
      });

  CHECK(asked);
  CHECK(result.ai_declined);
  CHECK(eval.evaluated.empty());
}

TEST_CASE("curate --ai gate returning false writes absolutely nothing and is distinguishable from an empty pool") {
  auto fx = make_singletons("eval_gate_declined", 6);
  FakeEvaluator eval;

  auto declined = detail::curate_impl(
      fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0, /*ai_enabled=*/true,
      Provider::Local, pzt::core::ai::LocalModelConfig{}, std::ref(eval), nullptr, nullptr, nullptr,
      [](int, int) { return false; });

  CHECK(declined.ai_declined);
  CHECK_FALSE(declined.cancelled);
  CHECK(declined.selected.empty());
  CHECK(declined.returned == 0);
  CHECK(declined.requested == 2);
  CHECK(eval.evaluated.empty());
  for (auto id : fx.images) CHECK_FALSE(has_evaluation(fx.db, id));

  // PRD 决策十九的整条理由就在这两行：拒绝之前，"selected 为空"唯一的含
  // 义是"这个项目里没有可选的照片"。两者必须能分辨，否则用户点了"不跑"
  // 会收到一句"没选出照片"。
  // 候选池真的为空：唯一那张图被打了废片标签，排除之后一个候选都不剩。
  auto empty_fx = make_singletons("eval_gate_empty_pool", 1);
  auto reject_tag = ensure_reject_tag(empty_fx.db, empty_fx.project_id);
  REQUIRE(add_tag(empty_fx.db, empty_fx.images[0], reject_tag).ok());
  auto empty = detail::curate_impl(empty_fx.db, empty_fx.project_id, std::nullopt, /*count=*/2, 20,
                                    10, 2.0, /*ai_enabled=*/true, Provider::Local,
                                    pzt::core::ai::LocalModelConfig{}, std::ref(eval));
  CHECK(empty.selected.empty());
  CHECK_FALSE(empty.ai_declined);
  CHECK_FALSE(empty.cancelled);
}

TEST_CASE("curate --ai reports per-photo evaluation progress, counted from 1 up to the real total") {
  auto fx = make_singletons("eval_progress", 12);
  FakeEvaluator eval;

  std::vector<std::pair<int, int>> progress;
  // 报在**发起之前**：每次回调时，已评估数应当正好比 done 少 1。
  detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                       /*ai_enabled=*/true, Provider::Local, pzt::core::ai::LocalModelConfig{},
                       std::ref(eval), nullptr, nullptr, nullptr, nullptr, [&](int done, int total) {
                         CHECK(static_cast<int>(eval.evaluated.size()) == done - 1);
                         progress.push_back({done, total});
                       });

  REQUIRE(progress.size() == 4);
  for (std::size_t i = 0; i < progress.size(); ++i) {
    CHECK(progress[i].first == static_cast<int>(i) + 1);  // 1-based，逐张递增
    CHECK(progress[i].second == 4);                        // total 全程不变
  }
}

TEST_CASE("curate --ai cancellation mid-evaluation is reported as cancelled, not as an empty pool") {
  auto fx = make_singletons("eval_cancel", 12);
  FakeEvaluator eval;

  // 粘性取消：评估过两张之后开始喊停(CancelFn 的契约要求一旦为真就一直
  // 为真，见 dedup.h)。
  bool stop = false;
  auto result = detail::curate_impl(
      fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0, /*ai_enabled=*/true,
      Provider::Local, pzt::core::ai::LocalModelConfig{},
      [&](Database& db, ImageId id) {
        bool ok = eval(db, id);
        if (eval.evaluated.size() >= 2) stop = true;
        return ok;
      },
      nullptr, nullptr, nullptr, nullptr, nullptr, [&] { return stop; });

  CHECK(result.cancelled);
  CHECK_FALSE(result.ai_declined);
  CHECK(result.selected.empty());
  CHECK(result.returned == 0);
  CHECK(eval.evaluated.size() == 2);  // 喊停之后不再发起新的评估
}

TEST_CASE("curate with ai disabled neither gates nor evaluates nor reports evaluation progress") {
  auto fx = make_singletons("eval_ai_off", 6);
  FakeEvaluator eval;

  bool gated = false;
  bool progressed = false;
  auto result = detail::curate_impl(
      fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0, /*ai_enabled=*/false,
      Provider::Local, pzt::core::ai::LocalModelConfig{}, std::ref(eval), nullptr, nullptr, nullptr,
      [&](int, int) {
        gated = true;
        return false;  // 万一被问到，返回 false 会让结果明显不对
      },
      [&](int, int) { progressed = true; });

  CHECK(result.returned == 2);
  CHECK_FALSE(gated);
  CHECK_FALSE(progressed);
  CHECK(eval.evaluated.empty());
  CHECK_FALSE(result.ai_declined);
  CHECK_FALSE(result.cancelled);
}

TEST_CASE("curate --ai carries on when a single evaluation fails") {
  // 一张图评估失败只是它没有描述可用(票 06 起由选择那一步处理)，不该让
  // 整批选片失败 - 跟锦标赛里"某簇比较失败就那一簇退化、不中断其它簇"是
  // 同一个立场。
  auto fx = make_singletons("eval_failure", 12);
  FakeEvaluator eval;
  eval.succeed = false;

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                     /*ai_enabled=*/true, Provider::Local,
                                     pzt::core::ai::LocalModelConfig{}, std::ref(eval));

  CHECK(eval.evaluated.size() == 4);  // 四张都试过
  CHECK(result.returned == 2);        // 选片照常交付
  CHECK_FALSE(result.cancelled);
  CHECK_FALSE(result.ai_declined);
}

TEST_CASE("curate --ai does not evaluate when the candidate pool is smaller than count") {
  // 候选不足 count 时没有"选"这个动作(全部返回)，也就没有预选集，因此没
  // 有任何评估开销 - 闸门报 0 张，一次请求都不发。
  auto fx = make_singletons("eval_shortfall", 2);
  FakeEvaluator eval;

  bool gated = false;
  auto result = detail::curate_impl(
      fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10, 2.0, /*ai_enabled=*/true,
      Provider::Local, pzt::core::ai::LocalModelConfig{}, std::ref(eval), nullptr, nullptr, nullptr,
      [&](int, int evaluations) {
        gated = true;
        CHECK(evaluations == 0);
        return true;
      });

  CHECK(result.returned == 2);
  CHECK(eval.evaluated.empty());
  CHECK_FALSE(gated);  // 开销恒为 0，不打扰调用方
}

TEST_CASE("curate --ai gate reports the cache-adjusted count, not the raw preselection size") {
  // 第一趟把预选集评估完；第二趟改成 count=3(预选集变大，多出两张没评估
  // 过的)。闸门此刻该报的是"还要评估几张"，不是预选集的总大小 - 已经有
  // 记录的那几张一次请求都不会发。
  auto fx = make_singletons("eval_gate_warm_cache", 12);
  FakeEvaluator warm;
  detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                       /*ai_enabled=*/true, Provider::Local, pzt::core::ai::LocalModelConfig{},
                       std::ref(warm));
  REQUIRE(warm.evaluated.size() == 4);

  FakeEvaluator again;
  int seen_evaluations = -1;
  detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/3, 20, 10, 2.0,
                       /*ai_enabled=*/true, Provider::Local, pzt::core::ai::LocalModelConfig{},
                       std::ref(again), nullptr, nullptr, nullptr, [&](int, int evaluations) {
                         seen_evaluations = evaluations;
                         return true;
                       });

  // 预选集是 6 张，其中 4 张(上一趟那批)已有记录 - 但两次的预选集不一定
  // 完全重叠，所以只断言闸门报的数跟真实发起的次数**相等**，这正是本票
  // "闸门报出的评估张数与实际执行的数量一致"要的。
  CHECK(seen_evaluations == static_cast<int>(again.evaluated.size()));
  CHECK(seen_evaluations < 6);  // 确实扣掉了缓存，不是原样报预选集大小
}

// ---------------------------------------------------------------------------
// 票 06：模型连选带排接进 curate
//
// 这一组同样走 detail::curate_impl 注入假 select_fn。本票要覆盖的行为(校
// 验、排序、退化分界)**全部只存在于成功路径上**，而这个文件既有的 AI 用例
// 一律是"让调用必然失败、只验证退化"-照抄等于零覆盖(PRD 测试决策的现状
// 警告)。
// ---------------------------------------------------------------------------

namespace {

// 记账用的假选择：把模型看到的候选录下来，按需返回一组序号或者汇报调用失
// 败(nullopt)。
struct FakeSelector {
  std::vector<pzt::core::ai::SelectionCandidate> seen;
  std::optional<std::vector<int>> reply;
  int calls = 0;
  int seen_count = -1;

  std::optional<std::vector<int>> operator()(
      const std::vector<pzt::core::ai::SelectionCandidate>& candidates, int count) {
    seen = candidates;
    seen_count = count;
    ++calls;
    return reply;
  }
};

}  // namespace

TEST_CASE("curate --ai delivers exactly the photos the model picked, in the model's order") {
  auto fx = make_singletons("select_order", 12);
  FakeEvaluator eval;
  FakeSelector select;
  select.reply = std::vector<int>{3, 1};  // count=2，预选集 4 张

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                     /*ai_enabled=*/true, Provider::Local,
                                     pzt::core::ai::LocalModelConfig{}, std::ref(eval),
                                     std::ref(select));

  // 冷缓存下评估顺序就是预选集顺序，也就是模型看到的 1..K 的编号顺序。
  REQUIRE(eval.evaluated.size() == 4);
  CHECK(select.calls == 1);
  CHECK(select.seen_count == 2);
  CHECK(result.returned == 2);
  // 顺序即交付顺序(PRD 决策十四)：3 在前、1 在后，不重排成时间序。
  CHECK(result.selected == std::vector<ImageId>{eval.evaluated[2], eval.evaluated[0]});
  CHECK_FALSE(result.ai_selection_fallback);
  // 选择结果不再来自 std::sample：同样的输入重复跑，结果逐字相同。
  FakeEvaluator eval2;
  FakeSelector select2;
  select2.reply = std::vector<int>{3, 1};
  auto again = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                    /*ai_enabled=*/true, Provider::Local,
                                    pzt::core::ai::LocalModelConfig{}, std::ref(eval2),
                                    std::ref(select2));
  CHECK(again.selected == result.selected);
}

TEST_CASE("curate --ai hands the model both the quality assessment and the content description") {
  auto fx = make_singletons("select_candidates", 12);
  FakeEvaluator eval;
  FakeSelector select;
  select.reply = std::vector<int>{1, 2};

  detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                       /*ai_enabled=*/true, Provider::Local, pzt::core::ai::LocalModelConfig{},
                       std::ref(eval), std::ref(select));

  // PRD 决策六：两个字段一起。候选的条数与顺序跟预选集一一对应(模型返回的
  // 序号靠这个对应关系翻译回照片)。
  REQUIRE(select.seen.size() == 4);
  for (const auto& c : select.seen) {
    CHECK(c.assessment == "锐利、构图均衡");
    CHECK(c.content == "一只猫趴在窗台上");
  }
}

TEST_CASE("curate --ai drops out-of-range and duplicate picks but still uses the valid ones") {
  auto fx = make_singletons("select_cleanup", 12);
  FakeEvaluator eval;
  FakeSelector select;
  // 4 张预选集里：99 越界、3 重复。清洗后剩 {3,1}，够 count=2，采纳。
  select.reply = std::vector<int>{3, 99, 3, 1, 0};

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                     /*ai_enabled=*/true, Provider::Local,
                                     pzt::core::ai::LocalModelConfig{}, std::ref(eval),
                                     std::ref(select));

  REQUIRE(eval.evaluated.size() == 4);
  CHECK(result.selected == std::vector<ImageId>{eval.evaluated[2], eval.evaluated[0]});
  CHECK_FALSE(result.ai_selection_fallback);  // 局部不合法不算整批退化
}

TEST_CASE("curate --ai falls back wholesale when too few picks survive cleaning") {
  auto fx = make_singletons("select_fallback_short", 12);
  FakeEvaluator eval;
  FakeSelector select;
  select.reply = std::vector<int>{1, 77};  // 清洗后只剩 1 个，count=2 不够

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                     /*ai_enabled=*/true, Provider::Local,
                                     pzt::core::ai::LocalModelConfig{}, std::ref(eval),
                                     std::ref(select));

  CHECK(result.ai_selection_fallback);
  CHECK(result.returned == 2);
  // 退化成**确定性**选择，不是拿模型那一个再补一张(PRD 决策十三：补进来的
  // 照片既不符合用户偏好、也不在模型排的顺序里，交付的会是两套逻辑拼接的
  // 结果，而用户看到的话术只有一种)。这里断言它跟关 AI 走同一条路的结果
  // 逐字相同 - 关 AI 那条路自己有独立用例钉着行为。
  FakeEvaluator unused;
  FakeSelector never;
  auto deterministic = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10,
                                            2.0, /*ai_enabled=*/false, Provider::Local,
                                            pzt::core::ai::LocalModelConfig{}, std::ref(unused),
                                            std::ref(never));
  CHECK(result.selected == deterministic.selected);
  CHECK(never.calls == 0);
}

TEST_CASE("curate --ai falls back wholesale when the selection call itself fails") {
  auto fx = make_singletons("select_fallback_error", 12);
  FakeEvaluator eval;
  FakeSelector select;  // reply 保持 nullopt = 网络/解析失败

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                     /*ai_enabled=*/true, Provider::Local,
                                     pzt::core::ai::LocalModelConfig{}, std::ref(eval),
                                     std::ref(select));

  CHECK(select.calls == 1);
  CHECK(result.ai_selection_fallback);
  CHECK(result.returned == 2);
  CHECK_FALSE(result.ai_declined);
  CHECK_FALSE(result.cancelled);
}

TEST_CASE("curate --ai keeps the whole-batch fallback signal independent from ai_fallback_count") {
  // PRD 决策二十一：ai_fallback_count 的语义是"**整簇**因为比较失败退化"，
  // 已经进了用户话术("哪几组不是 AI 挑的")；整批的选择退化是另一回事，混
  // 进同一个数字会让那句话直接说错。这条用例让两者同时可观测：两个 size>=2
  // 的簇因为没有 API key 必然比较失败(ai_fallback_count=2)，而选择这一步
  // 成功(ai_selection_fallback=false)。
  EnvVarGuard key("ANTHROPIC_API_KEY", nullptr);
  auto fx = make_fixture("select_signal_independence", 6);
  auto dir = fs::path(fx.root_path);
  for (char name : {'a', 'b', 'c', 'd', 'e', 'f'}) {
    REQUIRE(write_solid_jpeg(dir / (std::string(1, name) + ".jpg"), 16, 16, 120));
  }
  set_captured_at(fx.db, fx.images[0], 1000);
  set_captured_at(fx.db, fx.images[1], 1010);    // 跟 a 同簇
  set_captured_at(fx.db, fx.images[2], 100000);
  set_captured_at(fx.db, fx.images[3], 100010);  // 跟 c 同簇
  set_captured_at(fx.db, fx.images[4], 200000);
  set_captured_at(fx.db, fx.images[5], 300000);

  FakeEvaluator eval;
  FakeSelector select;
  select.reply = std::vector<int>{2, 1};

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                     /*ai_enabled=*/true, Provider::Claude,
                                     pzt::core::ai::LocalModelConfig{}, std::ref(eval),
                                     std::ref(select));

  CHECK(result.ai_fallback_count == 2);
  CHECK_FALSE(result.ai_selection_fallback);
  CHECK(result.returned == 2);

  // 反过来也成立：选择整批退化时，ai_fallback_count 不跟着变。
  FakeEvaluator eval2;
  FakeSelector failing;
  auto degraded = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                       /*ai_enabled=*/true, Provider::Claude,
                                       pzt::core::ai::LocalModelConfig{}, std::ref(eval2),
                                       std::ref(failing));
  CHECK(degraded.ai_fallback_count == 2);
  CHECK(degraded.ai_selection_fallback);
}

TEST_CASE("curate with ai disabled never consults the model and never reports a selection fallback") {
  auto fx = make_singletons("select_ai_off", 6);
  FakeEvaluator eval;
  FakeSelector select;
  select.reply = std::vector<int>{1, 2};  // 万一被问到，结果会明显不对

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0,
                                     /*ai_enabled=*/false, Provider::Local,
                                     pzt::core::ai::LocalModelConfig{}, std::ref(eval),
                                     std::ref(select));

  CHECK(select.calls == 0);
  CHECK_FALSE(result.ai_selection_fallback);
  CHECK(result.returned == 2);
  CHECK(result.selected == std::vector<ImageId>{fx.images[5], fx.images[0]});
}

TEST_CASE("curate --ai does not consult the model when the candidate pool is smaller than count") {
  // 候选不足 count 时没有"选"这个动作(全部返回、确定性排序)，也就没有可
  // 挑的余地 - 一次调用都不该发。
  auto fx = make_singletons("select_shortfall", 2);
  FakeEvaluator eval;
  FakeSelector select;
  select.reply = std::vector<int>{1};

  auto result = detail::curate_impl(fx.db, fx.project_id, std::nullopt, /*count=*/5, 20, 10, 2.0,
                                     /*ai_enabled=*/true, Provider::Local,
                                     pzt::core::ai::LocalModelConfig{}, std::ref(eval),
                                     std::ref(select));

  CHECK(select.calls == 0);
  CHECK(result.returned == 2);
  CHECK_FALSE(result.ai_selection_fallback);
  CHECK(result.selected == std::vector<ImageId>{fx.images[1], fx.images[0]});
}

TEST_CASE("curate --ai does not consult the model after the caller cancels or declines") {
  auto fx = make_singletons("select_not_after_stop", 12);

  FakeEvaluator declined_eval;
  FakeSelector declined_select;
  declined_select.reply = std::vector<int>{1, 2};
  auto declined = detail::curate_impl(
      fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0, /*ai_enabled=*/true,
      Provider::Local, pzt::core::ai::LocalModelConfig{}, std::ref(declined_eval),
      std::ref(declined_select), nullptr, nullptr, [](int, int) { return false; });
  CHECK(declined.ai_declined);
  CHECK(declined_select.calls == 0);
  CHECK_FALSE(declined.ai_selection_fallback);  // 没跑过就谈不上退化

  FakeEvaluator cancel_eval;
  FakeSelector cancel_select;
  cancel_select.reply = std::vector<int>{1, 2};
  bool stop = false;
  auto cancelled = detail::curate_impl(
      fx.db, fx.project_id, std::nullopt, /*count=*/2, 20, 10, 2.0, /*ai_enabled=*/true,
      Provider::Local, pzt::core::ai::LocalModelConfig{},
      [&](Database& db, ImageId id) {
        bool ok = cancel_eval(db, id);
        if (cancel_eval.evaluated.size() >= 2) stop = true;
        return ok;
      },
      std::ref(cancel_select), nullptr, nullptr, nullptr, nullptr, [&] { return stop; });
  CHECK(cancelled.cancelled);
  CHECK(cancel_select.calls == 0);
  CHECK_FALSE(cancelled.ai_selection_fallback);
}
