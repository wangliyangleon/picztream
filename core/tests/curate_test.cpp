#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

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
// 测试决策的第二条缝隙)——这一块最容易写错的是钳制的三个方向(下界
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
// 者胜出 -> c。随机采样只在这 3 张里发生，b/d/e 永远不会入选——不裁剪的
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
// 量贪心，"先挑 K 张再从这 K 张里挑 N 张"与"直接挑 N 张"的前 N 步完全同
// 序(每一步的 argmax 都落在 K 里)，所以裁剪在这条路上外部不可观测——这
// 条用例守的就是这个不变量，而不是某个新行为。
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
         /*ai_enabled=*/true, Provider::Claude, pzt::core::ai::LocalModelConfig{}, nullptr,
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
