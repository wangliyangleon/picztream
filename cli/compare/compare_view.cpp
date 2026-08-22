#include "cli/compare/compare_view.h"

#include <unistd.h>

#include <algorithm>
#include <string>

#include "cli/i18n/i18n.h"
#include "cli/term/screen.h"
#include "cli/text/text.h"
#include "cli/ui/ui.h"
#include "core/api.h"

namespace pzt::cli::compare {

namespace {

// 图片区四周留一点空隙,不让画面贴着屏幕边和栏间的缝 - 两张图贴在一起时
// 眼睛很难判断"这一块暗部属于左边还是右边"。
constexpr int kPadCols = 1;
constexpr int kPadRows = 1;
// 栏间距是奇数,中间那一列放分隔线,两侧各留一列空白 - 分隔线紧贴着某一
// 张图会读成"这张图的边框",而不是"两张图的分界"。
constexpr int kGapCols = 3;
constexpr int kProgressRows = 1;
constexpr const char* kDividerChar = "│";

// 终端没上报 cell 像素尺寸时的兜底,跟浏览主循环用的是同一组数:算出来的
// 画面会略有偏差,但不会除零,也不会把画面退化成一条线。
constexpr int kFallbackCellW = 8;
constexpr int kFallbackCellH = 16;

}  // namespace

CompareKey classify_key(char c) {
  switch (c) {
    case 'h':
      return CompareKey::PickLeft;
    case 'l':
      return CompareKey::PickRight;
    case 0x1B:
      return CompareKey::RequestAbandon;
    default:
      return CompareKey::Ignored;
  }
}

CompareChoice run_compare_loop(const DrawPairFn& draw_pair, const ReadKeyFn& read_key,
                                const ConfirmAbandonFn& confirm_abandon) {
  draw_pair();
  for (;;) {
    auto key = read_key();
    if (!key) return CompareChoice::Abandon;
    switch (classify_key(*key)) {
      case CompareKey::PickLeft:
        return CompareChoice::Left;
      case CompareKey::PickRight:
        return CompareChoice::Right;
      case CompareKey::RequestAbandon:
        if (confirm_abandon()) return CompareChoice::Abandon;
        // 确认提示盖住了屏幕底部,回到这一场得把画面恢复回来。这一场本身
        // 没有结论,进度不动。
        draw_pair();
        break;
      case CompareKey::Ignored:
        break;
    }
  }
}

CompareLayout compute_layout(const TerminalGeometry& term, int left_w, int left_h, int right_w,
                              int right_h) {
  CompareLayout out;
  const int cols = std::max(1, term.cols);
  const int rows = std::max(1, term.rows);
  const int cell_w = term.cell_px_w > 0 ? term.cell_px_w : kFallbackCellW;
  const int cell_h = term.cell_px_h > 0 ? term.cell_px_h : kFallbackCellH;

  out.progress_row = rows;
  out.progress_col = 1;
  out.progress_cols = cols;

  // 横向让步顺序:留白和栏间距一起让出去。让完还是不够宽,就是真的画不下。
  int pad_cols = kPadCols;
  int gap_cols = kGapCols;
  int pane_cols = (cols - pad_cols * 2 - gap_cols) / 2;
  if (pane_cols < 1) {
    pad_cols = 0;
    gap_cols = 0;
    pane_cols = cols / 2;
  }

  // 纵向同理,但进度行那一行永远保留 - 界面可以没有图,不能没有"这是第几
  // 场"。
  int pad_rows = kPadRows;
  int image_rows = rows - kProgressRows - pad_rows * 2;
  if (image_rows < 1) {
    pad_rows = 0;
    image_rows = rows - kProgressRows;
  }

  if (pane_cols < 1 || image_rows < 1) return out;

  const int image_top_row = 1 + pad_rows;
  const int left_box_col = 1 + pad_cols;
  const int right_box_col = left_box_col + pane_cols + gap_cols;

  if (gap_cols > 0) {
    out.divider_col = left_box_col + pane_cols + gap_cols / 2;
    out.divider_top_row = image_top_row;
    out.divider_rows = image_rows;
  }

  auto place = [&](int w, int h, int box_col) {
    PanePlacement p;
    if (w <= 0 || h <= 0) return p;
    auto fit = kitty::fit_within(w, h, pane_cols * cell_w, image_rows * cell_h);
    if (fit.width <= 0 || fit.height <= 0) return p;
    p.px_w = fit.width;
    p.px_h = fit.height;
    // 像素尺寸换算成 cell 数时向下取整会丢掉不足一个 cell 的余数,所以实
    // 际显示的长宽比与原图有不超过一个 cell 的偏差。这是 cell 网格本身的
    // 粒度,不是算错了。
    p.cols = std::clamp(fit.width / cell_w, 1, pane_cols);
    p.rows = std::clamp(fit.height / cell_h, 1, image_rows);
    p.col = box_col + (pane_cols - p.cols) / 2;
    p.row = image_top_row + (image_rows - p.rows) / 2;
    return p;
  };

  out.left = place(left_w, left_h, left_box_col);
  out.right = place(right_w, right_h, right_box_col);
  return out;
}

pzt::core::Result<void, kitty::RenderError> draw_pair_frame(
    int fd, const kitty::TerminalMode& mode, const CompareLayout& layout,
    const pzt::core::decode::DecodedImage& left, const pzt::core::decode::DecodedImage& right,
    int left_image_id, int right_image_id, const std::string& progress_line, std::size_t& frame) {
  using Result = pzt::core::Result<void, kitty::RenderError>;

  // 两条清除都排在两条传输之前,不是"清一张画一张":后者在换到尺寸不同的
  // 下一对时,左边新图已经画出来了,右边旧图还在原地,中间态肉眼可见。
  //
  // 清除失败不特殊处理 - 紧接着就要往同一个 id 写新的 placement,没有比
  // "继续往下走"更好的补救动作。
  (void)kitty::clear_placement(fd, mode, left_image_id);
  (void)kitty::clear_placement(fd, mode, right_image_id);

  // 分隔线整条一次写完:逐行一次 write 的话,这条线会肉眼可见地从上往下长
  // 出来。它落在两栏之间的空隙里,跟任何一张图都不重叠。
  if (layout.divider_col > 0) {
    // 第一行的定位由 write_at 负责,后面每一行自带一次定位:一列竖线在终端
    // 上不是连续的字节,每往下一行都得把光标重新拉回同一列。
    std::string divider = kDividerChar;
    for (int i = 1; i < layout.divider_rows; ++i) {
      divider += "\x1b[" + std::to_string(layout.divider_top_row + i) + ";" +
                 std::to_string(layout.divider_col) + "H" + kDividerChar;
    }
    ui::write_at(fd, layout.divider_top_row, layout.divider_col, divider);
  }

  auto draw_one = [&](const PanePlacement& p, const pzt::core::decode::DecodedImage& img,
                      int image_id) -> Result {
    if (p.cols <= 0 || p.rows <= 0) return Result::Ok();  // 这一栏画不下
    // 先在本地降采样到目标尺寸再发给终端:整张原始分辨率的 RGBA 丢过去让
    // 终端自己缩,是真机实测出来的切图卡顿来源。缩放失败就发原图,画面还
    // 在,只是慢一点。
    auto resized = pzt::core::resize_rgba(img, p.px_w, p.px_h);
    const auto& to_render = resized.ok() ? resized.value() : img;
    ui::write_at(fd, p.row, p.col, "");
    std::string tmp_path =
        kitty::make_tmp_path(std::to_string(getpid()) + "_cmp_" + std::to_string(frame++));
    return kitty::render_rgba_via_tmpfile(fd, mode, to_render, image_id, tmp_path, p.cols, p.rows);
  };

  auto drew_left = draw_one(layout.left, left, left_image_id);
  if (!drew_left.ok()) return drew_left;
  auto drew_right = draw_one(layout.right, right, right_image_id);
  if (!drew_right.ok()) return drew_right;

  ui::write_at(fd, layout.progress_row, layout.progress_col,
               text::pad_to(progress_line, static_cast<std::size_t>(layout.progress_cols)));
  return Result::Ok();
}

CompareView::CompareView(const kitty::TerminalMode& mode) : mode_(mode) {
  // 接管整块画面:上一个界面(浏览的三面板边框)的每一个字符都得走,否则边
  // 框会从两张图旁边露出来。图片区的残留由每帧的 clear_placement 负责,清
  // 屏只管文字。
  ui::write_stdout("\x1b[2J");
}

CompareView::~CompareView() {
  (void)kitty::clear_placement(STDOUT_FILENO, mode_, kLeftImageId);
  (void)kitty::clear_placement(STDOUT_FILENO, mode_, kRightImageId);
}

CompareChoice CompareView::compare(const pzt::core::decode::DecodedImage& left,
                                    const pzt::core::decode::DecodedImage& right,
                                    const std::string& progress_line, int comparisons_done) {
  // 每一场都重新量一次终端:比较是个能持续几分钟的动作,中途拉窗口不该让
  // 剩下的每一对都按旧尺寸画。这一场进行当中的拉窗口要等到下一对才生效,
  // 循环此刻正阻塞在读键上。
  auto size = pzt::cli::term::get_terminal_size();
  TerminalGeometry geom;
  geom.cols = size.valid ? size.cols : 80;
  geom.rows = size.valid ? size.rows : 24;
  geom.cell_px_w = size.valid ? std::max(1, size.pixel_width / size.cols) : 0;
  geom.cell_px_h = size.valid ? std::max(1, size.pixel_height / size.rows) : 0;

  auto layout = compute_layout(geom, left.width, left.height, right.width, right.height);

  auto draw_pair = [&] {
    (void)draw_pair_frame(STDOUT_FILENO, mode_, layout, left, right, kLeftImageId, kRightImageId,
                          progress_line, frame_);
  };

  auto confirm_abandon = [&] {
    int line1_row = std::max(1, layout.progress_row - 1);
    auto width = static_cast<std::size_t>(layout.progress_cols);
    ui::write_at(STDOUT_FILENO, line1_row, layout.progress_col,
                 text::pad_to(i18n::msg_pick_abandon_confirm_line1(comparisons_done), width));
    ui::write_at(STDOUT_FILENO, layout.progress_row, layout.progress_col,
                 text::pad_to(i18n::msg_pick_abandon_confirm_line2(), width));
    auto key = ui::read_one_byte_or_eof();
    // 提示的第一行落在图片区与进度行之间那道留白上,重画一帧不会盖掉它,
    // 得自己擦干净。
    ui::write_at(STDOUT_FILENO, line1_row, layout.progress_col, text::pad_to("", width));
    if (!key) return true;  // 没有人可以回答这个确认了
    return *key == 'y' || *key == 'Y';
  };

  return run_compare_loop(draw_pair, [] { return ui::read_one_byte_or_eof(); }, confirm_abandon);
}

}  // namespace pzt::cli::compare
