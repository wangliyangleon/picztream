#include <doctest.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "cli/compare/compare_view.h"
#include "cli/i18n/i18n.h"

using pzt::cli::compare::classify_key;
using pzt::cli::compare::CompareChoice;
using pzt::cli::compare::CompareKey;
using pzt::cli::compare::compute_layout;
using pzt::cli::compare::draw_pair_frame;
using pzt::cli::compare::PanePlacement;
using pzt::cli::compare::run_compare_loop;
using pzt::cli::compare::TerminalGeometry;

namespace {

// Ghostty 上一个典型的窗口:cell 8x17 像素。
TerminalGeometry typical_term() { return TerminalGeometry{160, 45, 8, 17}; }

// 一个 pane 完全落在屏幕里(1-based 行列,含端点)。
void check_in_bounds(const PanePlacement& p, const TerminalGeometry& term, int max_row) {
  CHECK(p.col >= 1);
  CHECK(p.row >= 1);
  CHECK(p.col + p.cols - 1 <= term.cols);
  CHECK(p.row + p.rows - 1 <= max_row);
}

// 目标像素尺寸与原图长宽比的相对偏差。fit_within 的整数取整会带来不超过
// 一个像素的误差,所以只能比到"接近",不能比相等。
double aspect_drift(const PanePlacement& p, int src_w, int src_h) {
  double want = static_cast<double>(src_w) / src_h;
  double got = static_cast<double>(p.px_w) / p.px_h;
  return std::abs(got - want) / want;
}

}  // namespace

TEST_CASE("两张横图并排:各自不越界、不变形、不重叠") {
  auto term = typical_term();
  auto layout = compute_layout(term, 6000, 4000, 6000, 4000);

  check_in_bounds(layout.left, term, layout.progress_row - 1);
  check_in_bounds(layout.right, term, layout.progress_row - 1);
  CHECK(layout.left.cols > 0);
  CHECK(layout.right.cols > 0);

  // 不重叠:左栏最后一列严格在右栏起始列之前。
  CHECK(layout.left.col + layout.left.cols - 1 < layout.right.col);

  // 不变形。
  CHECK(aspect_drift(layout.left, 6000, 4000) < 0.01);
  CHECK(aspect_drift(layout.right, 6000, 4000) < 0.01);

  // 一样的图一样的框,两栏该算出一样的尺寸。
  CHECK(layout.left.cols == layout.right.cols);
  CHECK(layout.left.rows == layout.right.rows);
}

TEST_CASE("一横一竖配在一起:两张各自保持自己的长宽比,竖图不把横图拖小") {
  auto term = typical_term();
  auto layout = compute_layout(term, 6000, 4000, 4000, 6000);

  CHECK(aspect_drift(layout.left, 6000, 4000) < 0.01);
  CHECK(aspect_drift(layout.right, 4000, 6000) < 0.01);

  // 竖图受高度限制、横图受宽度限制,所以横图应该更宽、竖图应该更高。
  CHECK(layout.left.cols > layout.right.cols);
  CHECK(layout.right.rows > layout.left.rows);

  check_in_bounds(layout.left, term, layout.progress_row - 1);
  check_in_bounds(layout.right, term, layout.progress_row - 1);
  CHECK(layout.left.col + layout.left.cols - 1 < layout.right.col);
}

TEST_CASE("进度行在最后一行,占满整宽,两张图都不压到它") {
  auto term = typical_term();
  auto layout = compute_layout(term, 6000, 4000, 4000, 6000);

  CHECK(layout.progress_row == term.rows);
  CHECK(layout.progress_col == 1);
  CHECK(layout.progress_cols == term.cols);
  CHECK(layout.left.row + layout.left.rows - 1 < layout.progress_row);
  CHECK(layout.right.row + layout.right.rows - 1 < layout.progress_row);
}

TEST_CASE("cell 是非正方形的:长宽比按像素算,不按 cell 数算") {
  // cell 8x17:正方形的图占的 cell 数必然是"列多于行",按 cell 数算长宽
  // 比会算成 1:1,那就是被拉长了。
  TerminalGeometry term{160, 45, 8, 17};
  auto layout = compute_layout(term, 3000, 3000, 3000, 3000);

  CHECK(aspect_drift(layout.left, 3000, 3000) < 0.01);
  CHECK(layout.left.cols > layout.left.rows);
}

TEST_CASE("列数是奇数时右栏也不越界") {
  TerminalGeometry term{81, 45, 8, 17};
  auto layout = compute_layout(term, 6000, 4000, 6000, 4000);

  check_in_bounds(layout.left, term, layout.progress_row - 1);
  check_in_bounds(layout.right, term, layout.progress_row - 1);
  CHECK(layout.left.col + layout.left.cols - 1 < layout.right.col);
}

TEST_CASE("终端小到极限也不算出负数或越界的几何") {
  for (int cols : {1, 2, 3, 5, 8, 20}) {
    for (int rows : {1, 2, 3, 4, 10}) {
      TerminalGeometry term{cols, rows, 8, 17};
      auto layout = compute_layout(term, 6000, 4000, 4000, 6000);
      CAPTURE(cols);
      CAPTURE(rows);
      CHECK(layout.progress_row == rows);
      CHECK(layout.progress_cols == cols);
      CHECK(layout.left.cols >= 0);
      CHECK(layout.right.cols >= 0);
      if (layout.left.cols > 0) check_in_bounds(layout.left, term, layout.progress_row - 1);
      if (layout.right.cols > 0) check_in_bounds(layout.right, term, layout.progress_row - 1);
      if (layout.left.cols > 0 && layout.right.cols > 0) {
        CHECK(layout.left.col + layout.left.cols - 1 < layout.right.col);
      }
      if (layout.divider_col > 0) {
        CHECK(layout.divider_col <= cols);
        CHECK(layout.divider_col > layout.left.col + layout.left.cols - 1);
        CHECK(layout.divider_col < layout.right.col);
        CHECK(layout.divider_top_row + layout.divider_rows - 1 < layout.progress_row);
      }
    }
  }
}

TEST_CASE("分隔线落在两栏之间,跟哪一张图都不重叠,高度跟图片区一致") {
  auto term = typical_term();
  auto layout = compute_layout(term, 6000, 4000, 4000, 6000);

  REQUIRE(layout.divider_col > 0);
  CHECK(layout.divider_col > layout.left.col + layout.left.cols - 1);
  CHECK(layout.divider_col < layout.right.col);
  // 两侧各留一列空白:分隔线紧贴着某一张图会被读成那张图的边框。
  CHECK(layout.divider_col > layout.left.col + layout.left.cols);
  CHECK(layout.divider_col + 1 < layout.right.col);
  CHECK(layout.divider_top_row >= 1);
  CHECK(layout.divider_top_row + layout.divider_rows - 1 < layout.progress_row);
}

TEST_CASE("终端窄到栏间距都让掉时没有分隔线,而不是画一条压在图上的线") {
  TerminalGeometry term{6, 20, 8, 17};
  auto layout = compute_layout(term, 6000, 4000, 6000, 4000);
  CHECK(layout.divider_col == 0);
  CHECK(layout.left.cols > 0);
  CHECK(layout.right.cols > 0);
  CHECK(layout.left.col + layout.left.cols - 1 < layout.right.col);
}

TEST_CASE("只剩一行时两栏都画不下,进度行仍然有位置") {
  TerminalGeometry term{160, 1, 8, 17};
  auto layout = compute_layout(term, 6000, 4000, 6000, 4000);
  CHECK(layout.left.cols == 0);
  CHECK(layout.right.cols == 0);
  CHECK(layout.progress_row == 1);
}

TEST_CASE("图片尺寸非正时那一栏的尺寸是 0,不是 1x1 的假图") {
  auto term = typical_term();
  auto layout = compute_layout(term, 0, 4000, 6000, 4000);
  CHECK(layout.left.cols == 0);
  CHECK(layout.left.rows == 0);
  CHECK(layout.right.cols > 0);
}

TEST_CASE("终端没上报 cell 像素尺寸时不除零") {
  TerminalGeometry term{160, 45, 0, 0};
  auto layout = compute_layout(term, 6000, 4000, 6000, 4000);
  CHECK(layout.left.cols >= 0);
  CHECK(layout.right.cols >= 0);
  check_in_bounds(layout.left, term, layout.progress_row - 1);
}

TEST_CASE("按键语义:h 选左、l 选右、Esc 请求放弃,其余忽略") {
  CHECK(classify_key('h') == CompareKey::PickLeft);
  CHECK(classify_key('l') == CompareKey::PickRight);
  CHECK(classify_key(0x1B) == CompareKey::RequestAbandon);
  for (char c : {'q', 'x', '1', 'y', 'j', 'k', ' ', '\n', 'H', 'L'}) {
    CAPTURE(c);
    CHECK(classify_key(c) == CompareKey::Ignored);
  }
}

namespace {

// 按脚本喂按键的假 ReadKeyFn:用完就返回 nullopt(EOF)。
struct ScriptedKeys {
  std::vector<std::optional<char>> keys;
  std::size_t next = 0;
  std::optional<char> operator()() {
    if (next >= keys.size()) return std::nullopt;
    return keys[next++];
  }
};

}  // namespace

TEST_CASE("h 选左、l 选右,各只画一帧") {
  ScriptedKeys keys{{'h'}};
  int draws = 0;
  int confirms = 0;
  auto choice = run_compare_loop([&] { ++draws; }, [&] { return keys(); },
                                  [&] {
                                    ++confirms;
                                    return true;
                                  });
  CHECK(choice == CompareChoice::Left);
  CHECK(draws == 1);
  CHECK(confirms == 0);

  ScriptedKeys keys2{{'l'}};
  draws = 0;
  auto choice2 = run_compare_loop([&] { ++draws; }, [&] { return keys2(); }, [&] { return true; });
  CHECK(choice2 == CompareChoice::Right);
  CHECK(draws == 1);
}

TEST_CASE("Esc 走二次确认,确认了才放弃") {
  ScriptedKeys keys{{static_cast<char>(0x1B)}};
  int confirms = 0;
  auto choice = run_compare_loop([] {}, [&] { return keys(); },
                                  [&] {
                                    ++confirms;
                                    return true;
                                  });
  CHECK(choice == CompareChoice::Abandon);
  CHECK(confirms == 1);
}

TEST_CASE("二次确认被取消:回到这一场重画,进度不倒退") {
  ScriptedKeys keys{{static_cast<char>(0x1B), 'l'}};
  int draws = 0;
  int confirms = 0;
  auto choice = run_compare_loop([&] { ++draws; }, [&] { return keys(); },
                                  [&] {
                                    ++confirms;
                                    return false;
                                  });
  // 放弃被取消之后这一场还在,用户接着按 l 就该选出右边,而不是这一场作废。
  CHECK(choice == CompareChoice::Right);
  CHECK(confirms == 1);
  // 确认提示盖掉了进度行,回来要重画一次:进入时一帧 + 回来一帧。
  CHECK(draws == 2);
}

TEST_CASE("未定义的按键被吞掉,不结束这一场也不重画") {
  ScriptedKeys keys{{'q', 'x', '1', 'y', ' ', 'h'}};
  int draws = 0;
  int confirms = 0;
  auto choice = run_compare_loop([&] { ++draws; }, [&] { return keys(); },
                                  [&] {
                                    ++confirms;
                                    return true;
                                  });
  CHECK(choice == CompareChoice::Left);
  CHECK(confirms == 0);
  // 画面没变,忽略掉的按键不该触发重画。
  CHECK(draws == 1);
}

TEST_CASE("stdin 到头:直接放弃,不去问一个没人能回答的确认") {
  ScriptedKeys keys{{}};
  int confirms = 0;
  auto choice = run_compare_loop([] {}, [&] { return keys(); },
                                  [&] {
                                    ++confirms;
                                    return false;
                                  });
  CHECK(choice == CompareChoice::Abandon);
  CHECK(confirms == 0);
}

namespace {

pzt::core::decode::DecodedImage tiny_image(int w, int h) {
  pzt::core::decode::DecodedImage img;
  img.width = w;
  img.height = h;
  img.rgba.assign(static_cast<std::size_t>(w) * h * 4, 200);
  return img;
}

std::string read_all(int fd) {
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<std::size_t>(n));
  return out;
}

}  // namespace

TEST_CASE("一帧的字节流:两条清除序列排在两条传输序列前面") {
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pzt::cli::kitty::TerminalMode mode;  // 独立窗口,不需要 passthrough
  auto layout = compute_layout(typical_term(), 400, 300, 300, 400);
  auto left = tiny_image(400, 300);
  auto right = tiny_image(300, 400);
  std::size_t frame = 0;

  auto result = draw_pair_frame(pipefd[1], mode, layout, left, right, /*left_image_id=*/11,
                                 /*right_image_id=*/12, "第 1/8 组", frame);
  close(pipefd[1]);
  CHECK(result.ok());

  std::string bytes = read_all(pipefd[0]);
  close(pipefd[0]);

  auto clear_left = bytes.find("a=d,d=i,q=2,i=11");
  auto clear_right = bytes.find("a=d,d=i,q=2,i=12");
  auto send_left = bytes.find(",i=11,");
  auto send_right = bytes.find(",i=12,");
  REQUIRE(clear_left != std::string::npos);
  REQUIRE(clear_right != std::string::npos);
  REQUIRE(send_left != std::string::npos);
  REQUIRE(send_right != std::string::npos);
  // 两条清除都要在两条传输之前 - 顺序反过来就是新图先画、再被自己的清
  // 除命令抹掉。
  CHECK(clear_left < send_left);
  CHECK(clear_left < send_right);
  CHECK(clear_right < send_left);
  CHECK(clear_right < send_right);

  // 进度行画在最后,内容原样。
  CHECK(bytes.find("第 1/8 组") != std::string::npos);
  CHECK(bytes.find("第 1/8 组") > send_right);

  // 分隔线每一行都画上了。
  REQUIRE(layout.divider_rows > 1);
  std::size_t divider_hits = 0;
  for (std::size_t at = bytes.find("│"); at != std::string::npos; at = bytes.find("│", at + 1)) {
    ++divider_hits;
  }
  CHECK(divider_hits == static_cast<std::size_t>(layout.divider_rows));

  // 每帧的临时文件路径要变,否则同一路径会被上一帧的终端读取行为影响。
  CHECK(frame == 2);
}

TEST_CASE("一帧的传输序列带上算好的 cell 数,不是让终端自己缩原图") {
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pzt::cli::kitty::TerminalMode mode;
  auto layout = compute_layout(typical_term(), 400, 300, 300, 400);
  auto left = tiny_image(400, 300);
  auto right = tiny_image(300, 400);
  std::size_t frame = 0;
  (void)draw_pair_frame(pipefd[1], mode, layout, left, right, 11, 12, "x", frame);
  close(pipefd[1]);
  std::string bytes = read_all(pipefd[0]);
  close(pipefd[0]);

  CHECK(bytes.find(",c=" + std::to_string(layout.left.cols) + ",r=" +
                   std::to_string(layout.left.rows)) != std::string::npos);
  CHECK(bytes.find(",c=" + std::to_string(layout.right.cols) + ",r=" +
                   std::to_string(layout.right.rows)) != std::string::npos);
}

TEST_CASE("放弃确认文案:说清作废几次、且明说不会有标签变化") {
  using namespace pzt::cli::i18n;
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    auto line1 = msg_pick_abandon_confirm_line1(7);
    CHECK(line1.find("7") != std::string::npos);
    CHECK_FALSE(msg_pick_abandon_confirm_line2().empty());
    // 已经比过 0 次时不该出现"作废 0 次"这种话。
    CHECK(msg_pick_abandon_confirm_line1(0).find("0") == std::string::npos);
  }
  g_lang = Lang::zh;
  CHECK(msg_pick_abandon_confirm_line1(7).find("标签") != std::string::npos);
  g_lang = Lang::en;
  CHECK(msg_pick_abandon_confirm_line1(7).find("tag") != std::string::npos);
  g_lang = Lang::zh;
}
