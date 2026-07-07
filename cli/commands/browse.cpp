#include "cli/commands/commands.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include "cli/kitty/kitty.h"
#include "cli/menu/filter_menu.h"
#include "cli/menu/recipe_menu.h"
#include "cli/menu/tag_menu.h"
#include "cli/term/cbreak_mode.h"
#include "cli/term/debug_log.h"
#include "cli/term/screen.h"
#include "cli/text/text.h"
#include "cli/ui/ui.h"
#include "cli/i18n/i18n.h"
#include "core/api.h"

// 搬过来的 cmd_open 函数体调用了 cli/text、cli/ui、cli/menu 里的一大堆函
// 数(pad_to/move_cursor/draw_hline/tags_for_menu/handle_space_key 等),
// 用 using-directive 让函数体保持逐字不变(.cpp 里用 using,头文件里绝不
// 用)。
using namespace pzt::cli::text;
using namespace pzt::cli::ui;
using namespace pzt::cli::menu;

namespace pzt::cli::commands {
namespace {

// 切换浏览池子(应用筛选/清除筛选)后 current_id 该是谁:能留在原地就留在
// 原地(原来那张图还在新池子里),留不住就退回列表头。两个方向复用同一条
// 规则,不为"进筛选"和"出筛选"分别定义两套语义。
pzt::core::ImageId resolve_current_after_switch(const std::vector<pzt::core::ImageRef>& new_images,
                                                 pzt::core::ImageId desired) {
  for (const auto& ref : new_images) {
    if (ref.id == desired) return desired;
  }
  return new_images.front().id;
}

}  // namespace

// increment 6.4.2:三面板固定布局(图片区左上约 80% 宽、信息栏右上、
// banner 底部全宽),备用屏幕缓冲区 + 每帧清除上一帧 placement,修复
// 6.4.1 真机测试时发现的图片重叠残留问题。
int cmd_open(const std::vector<std::string>& args) {
  bool debug_mode = false;
  std::vector<std::string> positional;
  for (const auto& a : args) {
    if (a == "--debug") {
      debug_mode = true;
    } else {
      positional.push_back(a);
    }
  }

  std::optional<pzt::core::ProjectId> id =
      positional.empty()
          ? pzt::core::find_project_by_root_path(std::filesystem::current_path().string())
          : pzt::core::find_project_by_name(positional[0]);
  if (!id) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_project_not_found().c_str());
    return 1;
  }

  auto opened = pzt::core::open_project(*id);
  if (!opened.ok()) {
    // id 来自刚成功的查找,理论上不该走到这里,但还是按"不假设它不会发生"
    // 的原则处理,而不是直接解引用。
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_project_not_found().c_str());
    return 1;
  }
  const auto& project = opened.value();

  auto images = pzt::core::list_images(*id);
  if (images.empty()) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_project_no_images(project.name).c_str());
    return 1;
  }

  // increment 6.4.5:废片系统标签正常应该在 pzt new 时就建好了,这里不是
  // 为了处理迁移——只是同一个幂等、廉价的 find-or-create,顺带兜住"项目
  // 不是通过更新后的 pzt new 建的"这种边界情况,避免后面用这个 id 时崩溃。
  pzt::core::TagId reject_tag_id = pzt::core::ensure_reject_tag(*id);

  auto mode = pzt::cli::kitty::detect_terminal_mode();
  if (mode.inside_tmux && !mode.passthrough_ok) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_tmux_passthrough().c_str());
    return 1;
  }

  const int kDebugRows = 8;
  std::size_t frame = 0;
  const int kImageId = 1;
  const int kBannerRows = 1;
  std::string banner_text = pzt::cli::i18n::banner_text();
  // j/k 转一整圈都没找到未打标签的图片时,不静默无反应——banner 这一帧显示
  // 这条提示而不是 kBannerText,显示完就清空,下一次不管按什么键都恢复正
  // 常提示。跟 current_id 一样是这个函数作用域内的纯局部状态,不需要额外
  // 的状态机或定时器。
  std::string status_override;

  // increment 6.4.7:退出时打一行 key-to-render 汇总(count/avg/p95/max)
  // ——PRD 验收标准要求"简单的延迟日志"验证浏览大量图片全程无可感知卡
  // 顿,盯着 debug 面板只保留最后 8 行的实时小窗口没法回头核对整个会话,
  // 需要一份事后能看的汇总,不是新的 core 层能力,纯粹是这个函数自己按
  // 键处理耗时的统计,声明在下面这个块外面,这样块结束(AltScreen/
  // CbreakMode 析构、stderr 换回真实终端)之后还能在这打印。退出时打印一
  // 次,不挂在 --debug 后面。
  std::size_t latency_count = 0;
  double latency_sum_ms = 0.0;
  double latency_max_ms = 0.0;
  std::vector<double> latency_samples;

  {
    // 默认把 stderr(core::PrefetchCache 等的延迟日志)整个丢掉,不跟图片画
    // 到同一块屏幕上;--debug 时改成后台收集,画到屏幕底部专门的 debug 区
    // 域。声明在 prefetch 之前、比它晚析构,这样 prefetch 关闭时可能打的最
    // 后几行日志也能被收住。跟 prefetch 一起放进这个块(而不是 cmd_open 的
    // 外层作用域)是 6.4.7 修的一个问题:这两个如果活到 cmd_open 整个函数
    // 返回才析构,块结束之后想打印的退出汇总/退出提示这些"应该在真实终端
    // 上可见"的输出,实际上还是会被 debug_log 占着的重定向吞掉(不管
    // --debug 开没开,DebugLogRedirect 的析构函数才是真正把 stderr 换回真
    // 实终端的地方)——缩小到这个块的作用域,块结束时 debug_log 先析构、
    // stderr 先换回来,后面的打印才真的能看见。
    pzt::cli::term::DebugLogRedirect debug_log(debug_mode, static_cast<std::size_t>(kDebugRows));

    // window 先给个保守默认值——PRD 里"合理默认值待真实素材测出"这个待办不
    // 受这次影响,调优留给以后有真实使用数据再说。
    pzt::core::PrefetchCache prefetch(project.root_path, /*window=*/3, pzt::core::decode_jpeg_file);
    pzt::core::ImageId current_id = images.front().id;
    prefetch.set_current(images, current_id);

    // AltScreen 在 CbreakMode 前构造、后析构:退出时先把输入模式还原、再离
    // 开备用缓冲区,这样即便中途出异常,用户的主屏幕内容也不会被半途切走
    // 又切不回来。
    pzt::cli::term::AltScreen alt_screen;
    pzt::cli::term::CbreakMode cbreak;

    // 上一帧实际渲染的是哪张图——打标签这类不改 current_id 的操作不需要
    // 重新拉取/传输图片本身,只有 current_id 真的变了才需要。
    std::optional<pzt::core::ImageId> last_rendered_id;

    // M1 increment 5:`r v` 临时切换当前图片是否展示风格化效果,纯查看层
    // 面的状态,不碰数据库。导航到新图片时重置为 false(默认展示风格化效
    // 果),只有 style_toggled 为真时才需要在 current_id 没变的情况下也
    // 强制重新走一遍渲染(正常情况下 current_id 不变就不需要重画)。
    bool show_original = false;
    bool style_toggled = false;

    // 上一帧是不是刚显示过 status_override 这种一次性提示——刚显示过的
    // 话,下一次读键不管读到什么都只用来"消除提示",不当成 h/l/j/k/space
    // 的具体动作处理,呼应提示文案里"按任意键继续"这句话:既然说了任意
    // 键,就不应该因为按的不是那几个认识的键就什么反应都没有,也不应该让
    // 这一次按键同时"消除提示"又"顺便导航/打开菜单",那样反而让人搞不清
    // 这次按键到底生效了没有。
    bool showing_status = false;

    // 上一轮是不是 --debug 模式下 poll 超时(没有真实按键)触发的重画——是
    // 的话,这一轮渲染完不打 key-to-render 延迟日志:这条日志的本意是"从
    // 按键到渲染完成"的延迟,超时触发的重画根本没有对应的按键,量出来的
    // 只是这一帧本身的渲染耗时(而且图片这步大概率被跳过,数字会很小),
    // 跟这条日志真正想回答的问题("切图快不快")没关系,混在一起只会让
    // debug 面板看起来像是在不停后台重复干活。
    bool suppress_latency_log = false;

    // increment 6.4.6:当前是否在 g + 数字切出来的筛选视图里,以及筛选到
    // 了哪个标签——跟 current_id 一样是这个函数作用域内的纯局部状态。
    std::optional<pzt::core::TagId> active_filter_tag_id;
    std::string active_filter_tag_name;

    while (true) {
      auto key_time = std::chrono::steady_clock::now();

      std::size_t index = 0;
      const pzt::core::ImageRef* current_ref = nullptr;
      for (std::size_t i = 0; i < images.size(); ++i) {
        if (images[i].id == current_id) {
          index = i;
          current_ref = &images[i];
          break;
        }
      }

      // 导航检测和 show_original 的重置要放在信息栏绘制之前(这一帧剩下
      // 的部分,包括信息栏和实际渲染,都要看到重置之后的值)——之前这个
      // reset 是在图片渲染那一段(信息栏之后)才做的,导致切到新图片的第
      // 一帧信息栏还在用上一张图片遗留的 show_original,画出"没加粗/没
      // 星号",要等下一帧才更新成正确的加粗状态,真机测试能明显看到这个
      // 卡顿。
      bool navigated = (last_rendered_id != current_id);
      if (navigated) {
        show_original = false;  // 每次导航到新图片,默认展示风格化效果
      }

      auto term_size = pzt::cli::term::get_terminal_size();
      // 拿不到真实尺寸(非 tty、或者终端没上报像素尺寸)时给一组保守的兜
      // 底值,不让布局计算除零或者算出负数区域。
      int total_cols = term_size.valid ? term_size.cols : 80;
      int total_rows = term_size.valid ? term_size.rows : 24;
      int cell_px_w = term_size.valid ? std::max(1, term_size.pixel_width / term_size.cols) : 8;
      int cell_px_h = term_size.valid ? std::max(1, term_size.pixel_height / term_size.rows) : 16;

      // 整个界面默认只占终端宽度的 70%、居中显示,不铺满整个窗口——以后加
      // 了冒号命令再考虑让这个比例可调。
      const double kWidthRatio = 0.7;
      int ui_cols = std::max(20, static_cast<int>(total_cols * kWidthRatio));
      int start_col = std::max(1, (total_cols - ui_cols) / 2 + 1);

      int content_cols = std::max(1, ui_cols - 2);  // 减去左右各一列边框
      int image_cols = std::max(1, static_cast<int>(content_cols * 0.8));
      int mid_offset = 1 + image_cols;  // 中间竖线相对 start_col 的偏移
      int info_cols = std::max(1, content_cols - image_cols - 2);  // -1: 中间竖线,-1: 留一列空隙
      int info_col = start_col + mid_offset + 2;  // 信息栏内容起始列,跳过竖线和一列空隙

      int border_rows = 2;  // 顶部 + 底部
      int divider_rows = 1 + (debug_mode ? 1 : 0);
      int fixed_rows = border_rows + divider_rows + kBannerRows + (debug_mode ? kDebugRows : 0);
      int top_rows = std::max(1, total_rows - fixed_rows);

      // 画边框:单个外框 + 图片/信息栏之间的竖线分隔,风格照抄设计阶段讨论
      // 过的 ASCII 示意图,不是四个各自独立的小方框。
      {
        int row = 1;
        draw_hline(row++, start_col, ui_cols, "┌", "┐", mid_offset, "┬");
        for (int i = 0; i < top_rows; ++i) draw_vlines(row + i, start_col, ui_cols, mid_offset);
        row += top_rows;
        draw_hline(row++, start_col, ui_cols, "├", "┤", mid_offset, "┴");
        if (debug_mode) {
          for (int i = 0; i < kDebugRows; ++i) draw_vlines(row + i, start_col, ui_cols);
          row += kDebugRows;
          draw_hline(row++, start_col, ui_cols, "├", "┤");
        }
        draw_vlines(row, start_col, ui_cols);
        row++;
        draw_hline(row, start_col, ui_cols, "└", "┘");
      }
      int image_top_row = 2;  // 顶部边框占第 1 行,图片/信息内容从第 2 行开始
      int debug_top_row = 2 + top_rows + 1;  // 图片区 + 分隔线之后
      int banner_row = debug_top_row + (debug_mode ? kDebugRows + 1 : 0);

      // 信息栏:编号、文件名、标签、文件大小,固定在图片区右侧。内容行数
      // 随标签数量变化(标签越多占的行越多)——真机测试发现,标签数变少之
      // 后,上一帧比较靠下的内容(比如"大小:"那一行)不会被这一帧覆盖到,
      // 会一直重影在那。先把整个信息栏区域清空,再画这一帧实际用到的内
      // 容,不管行数怎么变都不会留下上一帧的残留。
      {
        for (int r = 0; r < top_rows; ++r) {
          move_cursor(image_top_row + r, info_col);
          write_stdout(pad_to("", info_cols));
        }
        int row = image_top_row;
        move_cursor(row++, info_col);
        // increment 6.4.6:筛选状态拼在这一行后面,不新增一行——这样下面
        // 每一行(文件名、标签、大小)不管是不是在筛选视图里都是完全一样
        // 的行号计算,切换筛选状态时不会有内容跳动。
        std::string index_line =
            "[" + std::to_string(index + 1) + "/" + std::to_string(images.size()) + "]";
        if (active_filter_tag_id) index_line += pzt::cli::i18n::info_filter_label(active_filter_tag_name);
        write_stdout(pad_to(index_line, info_cols));

        move_cursor(row++, info_col);
        write_stdout(pad_to(current_ref ? current_ref->file_name : "?", info_cols));

        row++;  // 空一行
        move_cursor(row++, info_col);
        write_stdout(pad_to(pzt::cli::i18n::info_tags_label(), info_cols));
        auto tags = current_ref ? pzt::core::tags_for_image(current_ref->id)
                                 : std::vector<pzt::core::TagSummary>{};
        if (tags.empty()) {
          move_cursor(row++, info_col);
          write_stdout(pad_to(pzt::cli::i18n::info_none_label(), info_cols));
        } else {
          for (const auto& t : tags) {
            move_cursor(row++, info_col);
            write_stdout(pad_to(pzt::cli::i18n::tag_display_name(t), info_cols));
          }
        }

        row++;  // 空一行
        auto info = current_ref ? pzt::core::get_image(current_ref->id) : std::nullopt;
        if (info) {
          move_cursor(row++, info_col);
          write_stdout(pad_to(pzt::cli::i18n::info_size_label(format_size(info->file_size)), info_cols));
        }

        // M1 increment 3:在真正的 `r` 交互(increment 6)和预览渲染
        // (increment 5)落地之前,先在信息栏露出"这张图应用了哪个风格",
        // 方便用 apply-debug 之类的调试命令验证时能直观看到结果,不用每
        // 次都手动查数据库。两层模型(预设/version)用两级缩进画成一棵小
        // 树,不是拼成一行文本——真机测试发现拼一行会在信息栏这种窄列里
        // 被截断,例如"风格: Standard: MyStandard"就被切成了"风格:
        // Standard: MyStanda",看不全。
        row++;  // 空一行
        move_cursor(row++, info_col);
        write_stdout(pad_to(pzt::cli::i18n::info_style_label(), info_cols));
        auto recipe_id = current_ref ? pzt::core::get_image_recipe(current_ref->id) : std::nullopt;
        auto style = recipe_id ? pzt::core::describe_recipe(*recipe_id) : std::nullopt;
        if (!style) {
          move_cursor(row++, info_col);
          write_stdout(pad_to(pzt::cli::i18n::info_style_none_label(), info_cols));
        } else {
          // M1 increment 5:当前实际渲染的是风格化效果时标出来(`r v` 切
          // 到原图预览时取消),直接呼应"现在看到的是不是风格化效果"这个
          // 状态。真机测试发现单靠 ANSI 粗体(`\x1b[1m`)不可靠——很多终端
          // 的中文字体没有配置独立的粗体字重,ASCII 文本(比如预设名
          // "Origin")会正常加粗,但中文 version 名字(比如"亮一点")的字
          // 重不会变,两行看起来不一致,不是代码逻辑的问题,是终端/字体限
          // 制。改用不依赖字重的文字标记(`*`)当主要信号,粗体转义码还留
          // 着(在支持的终端上锦上添花),但不再是唯一的指示方式。标记跟
          // 缩进空格等宽替换,不破坏两级缩进的对齐。粗体转义码要包在
          // pad_to 算完显示宽度之后的结果外层,不能传给 pad_to 之前就
          // 包——不然转义字节会被 display_width 当成可见字符,算错截断/
          // 补空格的位置。
          bool active = !show_original;
          auto emit_style_line = [&](const std::string& indent, const std::string& text) {
            std::string marker = active ? indent.substr(0, indent.size() - 2) + "* " : indent;
            std::string padded = pad_to(marker + text, info_cols);
            write_stdout(active ? "\x1b[1m" + padded + "\x1b[0m" : padded);
          };
          move_cursor(row++, info_col);
          emit_style_line("  ", style->preset_name);
          if (style->version_name) {
            move_cursor(row++, info_col);
            emit_style_line("    ", *style->version_name);
          }
        }
      }

      // --debug 时,图片/信息栏下方专门留出来的滚动 debug 区——按帧重画最
      // 新的 kDebugRows 行,不是真正的终端滚动区域,但对用户来说效果一样:
      // 新日志进来,老的自然被挤出显示范围。
      if (debug_mode) {
        auto lines = debug_log.snapshot();
        std::size_t begin =
            lines.size() > static_cast<std::size_t>(kDebugRows)
                ? lines.size() - static_cast<std::size_t>(kDebugRows)
                : 0;
        for (int i = 0; i < kDebugRows; ++i) {
          move_cursor(debug_top_row + i, start_col + 1);
          std::size_t idx = begin + static_cast<std::size_t>(i);
          write_stdout(pad_to(idx < lines.size() ? lines[idx] : "", content_cols));
        }
      }

      // Banner:固定在图片/信息栏下方最后一行,边框内全宽。
      move_cursor(banner_row, start_col + 1);
      showing_status = !status_override.empty();
      if (showing_status) {
        // status_override 里的消息大多自带一个尾随空格(跟 kBannerText 的
        // 视觉留白风格一致),直接拼接"  按任意键继续"会在两者之间留出一大
        // 段空白,看起来像隔得很远——先去掉消息自己的尾随空格,用逗号衔接
        // 而不是额外的空格。
        std::string trimmed = status_override;
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        write_stdout(pad_to(pzt::cli::i18n::msg_press_any_key_to_continue(trimmed), content_cols));
      } else {
        write_stdout(pad_to(banner_text, content_cols));
      }
      status_override.clear();  // 只显示这一帧,不管接下来按了什么键都恢复正常提示

      // 图片放在信息栏/banner 之后画:真机测试反馈图片显示出来之后,右边
      // 信息栏和底部 banner 的文字有明显的滞后才跟上,怀疑是 Ghostty 处理
      // Kitty 图片协议命令(读临时文件、解码、合成)这一步在它自己的主循环
      // 里是同步/阻塞的,会顺带卡住紧跟在图片命令后面的文字——即便我们这
      // 边是几乎同时把所有这些控制序列写出去的。这几行文字本身很小、写
      // 出去的成本可以忽略,调整顺序让文字先于图片写出去,这样即便终端处
      // 理图片这一步确实慢,文字至少能立刻显示,不用跟着一起卡住。打标签
      // 这类操作不会改 current_id,不需要重新清除/传输同一张图——真机测试
      // 发现,不加这个判断的话,打个标签也会因为整帧重画而卡顿一下,尽管
      // 图片内容根本没变。只有 current_id 真的变了才重新走一遍"清掉上一
      // 帧的图 -> 取解码结果 -> 缩放 -> 传输"这一整套。`navigated` 在这
      // 一帧最前面(信息栏绘制之前)已经算过、`show_original` 也已经在
      // 那里重置过,这里直接复用,不重新算一遍。
      if (navigated || style_toggled) {
        // 每帧先清掉上一帧的图,再画新的——这是修复 6.4.1 重叠残留问题的
        // 关键一步,没有它,旧 placement 不会自动消失。
        pzt::cli::kitty::clear_placement(STDOUT_FILENO, mode, kImageId);

        auto decoded = prefetch.get(current_id);
        if (decoded.ok()) {
          const auto& img = decoded.value();
          // 让图片在面板里居中、四周留一点空隙,而不是贴着左边框/上边
          // 框——fit_within 只保证"不超出"这个框,不保证"居中",长宽比
          // 跟面板不完全匹配时(几乎总是这样)不作处理的话,多出来的空白
          // 会全部堆在右边/下边,图片贴着另外两条边。先从可用区域里减掉
          // 一份固定 padding 再传给 fit_within,保证贴得最紧的那个维度
          // 也留有空隙;再用算出来的目标尺寸相对完整的 image_cols x
          // top_rows 框计算居中偏移,把剩余的宽松空间平均分到两侧。
          const int kImagePaddingCols = 2;  // 终端 cell 不是正方形,横向
          const int kImagePaddingRows = 1;  // 留白数值上比纵向大一点,视觉才均衡
          int avail_cols = std::max(1, image_cols - kImagePaddingCols * 2);
          int avail_rows = std::max(1, top_rows - kImagePaddingRows * 2);
          auto fit = pzt::cli::kitty::fit_within(img.width, img.height, avail_cols * cell_px_w,
                                                  avail_rows * cell_px_h);
          int target_cols = std::max(1, fit.width / cell_px_w);
          int target_rows = std::max(1, fit.height / cell_px_h);
          int offset_cols = (image_cols - target_cols) / 2;
          int offset_rows = (top_rows - target_rows) / 2;

          // 真机测试确认过:每帧把原始分辨率的 RGBA(可能几 MB 到近十 MB)
          // 整个丢给终端,终端自己读临时文件+解码+缩放显示,是切图卡顿的
          // 实际来源——即便我们这边 prefetch 已经命中、解码耗时为 0。先
          // 在这边缩小到面板大致能显示的尺寸,大幅减少终端侧要处理的数
          // 据量。
          auto resized = pzt::core::resize_rgba(img, fit.width, fit.height);
          const auto& downsampled = resized.ok() ? resized.value() : img;

          // M1 increment 5:在降采样之后、发给终端之前应用 recipe。
          // thread_count=1 同步执行——Phase 0 spike 已经验证过预览分辨率
          // 下这一步足够便宜(10-22ms),不需要额外的后台线程或缓存;这个
          // if 块本来就只在导航或 `r v` 切换时才跑,不会每帧都重算。
          // show_original 为真时(用户按了 r v 切到原图预览)跳过渲染。
          std::optional<pzt::core::DecodedImage> styled;
          auto recipe_id = pzt::core::get_image_recipe(current_id);
          if (recipe_id && !show_original) {
            auto render_result = pzt::core::render(downsampled, *recipe_id, 1);
            if (render_result.ok()) styled = std::move(render_result.value());
            // render 失败(比如引用了一个数据损坏的 recipe_id)时静默回退
            // 到未处理的画面,不阻断浏览,跟"图片解码失败,跳过"是同一种
            // 防御精神。
          }
          const auto& to_render = styled ? *styled : downsampled;

          move_cursor(image_top_row + offset_rows, start_col + 1 + offset_cols);
          std::string tmp_path = pzt::cli::kitty::make_tmp_path(
              std::to_string(getpid()) + "_" + std::to_string(frame++));
          auto rendered = pzt::cli::kitty::render_rgba_via_tmpfile(
              STDOUT_FILENO, mode, to_render, kImageId, tmp_path, target_cols, target_rows);
          if (!rendered.ok()) {
            std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_render_failed().c_str());
          }
        } else {
          std::fprintf(stderr, "%s", pzt::cli::i18n::err_open_decode_failed().c_str());
        }
        last_rendered_id = current_id;
        style_toggled = false;
      }

      if (!suppress_latency_log) {
        double key_to_render_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - key_time)
                .count();
        std::fprintf(stderr, "[pzt open] key-to-render %.2fms\n", key_to_render_ms);
        ++latency_count;
        latency_sum_ms += key_to_render_ms;
        latency_max_ms = std::max(latency_max_ms, key_to_render_ms);
        latency_samples.push_back(key_to_render_ms);
      }

      char c = 0;
      if (showing_status) {
        // 刚显示过一次性提示("按任意键继续"),这一次读键不管读到什么字
        // 节都只用来消除提示、跳回外层循环重画一次正常画面,不当成
        // h/l/j/k/space 的具体动作执行——否则这一次按键会同时"消除提示"
        // 又"顺便导航/打开菜单",容易让人搞不清这次按键到底生效了没有。
        ssize_t n = read(STDIN_FILENO, &c, 1);
        showing_status = false;
        suppress_latency_log = false;  // 这一轮读到了真实按键(用来消除提示)
        if (n <= 0) break;  // 真正的 EOF/出错,当退出处理
        continue;
      }

      // 不支持的键直接在这个内层循环里吃掉,继续读下一个字节——不 continue
      // 回外层 while,那样会导致整个画面(边框、图片、信息栏、banner)重新
      // 渲染一遍,一次误按不支持的键就能看到明显的闪烁。--debug 模式下例
      // 外:每一轮先 poll 一次,超时(没有任何按键)就直接 continue 回外层
      // 重画,刷新 debug 面板;不开 --debug 时维持原来单纯阻塞 read 的写
      // 法,不引入 poll 的额外开销。
      bool timed_out = false;
      while (true) {
        if (debug_mode) {
          if (!stdin_ready(300)) {
            timed_out = true;
            break;
          }
        }
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
          c = 'q';
          break;
        }
        if (c == 'q' || c == 'h' || c == 'l' || c == 'j' || c == 'k' || c == ' ' || c == 'x' ||
            c == 'g' || c == 'r') {
          break;
        }
      }
      if (timed_out) {
        suppress_latency_log = true;  // 没有按键,只是刷新 debug 面板,不处理导航
        continue;
      }
      suppress_latency_log = false;  // 这一轮确实读到了真实按键
      if (c == 'q') break;

      if (c == 'h') {
        current_id = pzt::core::prev_image(images, current_id).value_or(current_id);
      } else if (c == 'l') {
        current_id = pzt::core::next_image(images, current_id).value_or(current_id);
      } else if (c == 'j') {
        // 筛选视图里每张图按定义都至少有筛选到的那个标签,"下一个未打标
        // 签的" 在这个语境下没有意义,永远立刻报"全部打完"——不是 bug,但
        // 体验上很尴尬,筛选生效时退化成跟 l 一样的普通下一张。
        if (active_filter_tag_id) {
          current_id = pzt::core::next_image(images, current_id).value_or(current_id);
        } else {
          auto next = pzt::core::next_untagged(images, current_id);
          if (next) {
            current_id = *next;
          } else {
            status_override = pzt::cli::i18n::msg_all_tagged();
          }
        }
      } else if (c == 'k') {
        if (active_filter_tag_id) {
          current_id = pzt::core::prev_image(images, current_id).value_or(current_id);
        } else {
          auto prev = pzt::core::prev_untagged(images, current_id);
          if (prev) {
            current_id = *prev;
          } else {
            status_override = pzt::cli::i18n::msg_all_tagged();
          }
        }
      } else if (c == ' ') {
        if (current_ref) {
          status_override = handle_space_key(*id, reject_tag_id, current_ref->id, banner_row,
                                              start_col, content_cols);
        }
        // current_id 不变,跟其它分支一样落到下面的 set_current + 循环顶部
        // 整屏重绘,信息栏会自然显示打标签之后的结果。
      } else if (c == 'x') {
        // 标记为废片的直达快捷键,等价于 space + 0/space - 0,但不用先开
        // 菜单——废片预期是使用频率最高的标签,值得单独开一个键。做成开
        // 关切换(已经标了就摘掉):误按一下能直接再按一次撤销,不需要先
        // 开 space 菜单走摘除流程。
        if (current_ref) {
          auto current_tags = pzt::core::tags_for_image(current_ref->id);
          bool already_tagged = std::any_of(
              current_tags.begin(), current_tags.end(),
              [&](const auto& t) { return t.id == reject_tag_id; });
          if (already_tagged) {
            auto result = pzt::core::remove_tag(current_ref->id, reject_tag_id);
            status_override = result.ok() ? "" : pzt::cli::i18n::err_remove_tag_failed();
          } else {
            status_override = handle_add_tag_result(reject_tag_id, current_ref->id, banner_row,
                                                     start_col, content_cols);
          }
        }
      } else if (c == 'g') {
        // g + 数字切换到只浏览该标签下图片的筛选视图,g + g 清除筛选回到
        // 完整项目——数字编号复用跟 space 菜单同一套 tags_for_menu。
        auto tags = tags_for_menu(*id);
        auto decision = handle_g_key_prompt(reject_tag_id, tags, active_filter_tag_id,
                                             active_filter_tag_name, banner_row, start_col,
                                             content_cols);

        if (decision.action == GKeyAction::Handled) {
          status_override = decision.status;
        } else if (decision.action == GKeyAction::ApplyFilter) {
          // 真机测试反馈 g + 数字筛选有明显卡顿,查出来是 image_tags 按
          // tag_id 过滤没有索引可用(见 core/db/schema.cpp 的说明,已经
          // 补上索引)——这里打一下查询本身的耗时,debug 面板能直接看到
          // 这一步占了多少,跟后面"切到新图片要重新解码"那部分区分开。
          auto filter_t0 = std::chrono::steady_clock::now();
          auto filtered = pzt::core::filter_by_tag(decision.tag_id);
          double filter_query_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - filter_t0)
                                        .count();
          std::fprintf(stderr, "[pzt open] filter_by_tag tag_id=%lld %.2fms\n",
                       static_cast<long long>(decision.tag_id), filter_query_ms);
          if (!filtered.ok()) {
            status_override = pzt::cli::i18n::err_filter_failed();  // 结构上不可能,防御性处理
          } else if (filtered.value().empty()) {
            status_override = pzt::cli::i18n::msg_filter_no_images();  // 拒绝切换,images/current_id 不变
          } else {
            // 注意顺序:先用 filtered.value() 算出 new_current,再 move,
            // 不然 move 之后 filtered.value() 已经是空壳。
            pzt::core::ImageId new_current =
                resolve_current_after_switch(filtered.value(), current_id);
            images = std::move(filtered.value());
            current_id = new_current;
            active_filter_tag_id = decision.tag_id;
            active_filter_tag_name = decision.tag_name;
          }
        } else if (decision.action == GKeyAction::ClearFilter) {
          if (active_filter_tag_id) {
            auto full = pzt::core::list_images(*id);
            pzt::core::ImageId new_current = resolve_current_after_switch(full, current_id);
            images = std::move(full);
            current_id = new_current;
            active_filter_tag_id.reset();
            active_filter_tag_name.clear();
          }
          // 不在筛选中时 g+g 是空操作:不查库、不提示,静默——避免每次误
          // 按 g+g 在未筛选状态下也触发一次不必要的 list_images 查询。
        }
        // Cancel:什么都不做,静默
      } else if (c == 'r') {
        // increment 6:完整的 `r` 前缀键交互,见 handle_r_key。应用/清除
        // 需要重新走一遍渲染(recipe_id 变了或者切到原图预览),交给
        // style_toggled 触发;创建/删除不影响当前图片的 recipe_id,不需
        // 要强制重画。
        if (current_ref) {
          auto outcome =
              handle_r_key(current_ref->id, banner_row, start_col, content_cols);
          status_override = outcome.status;
          if (outcome.action == RKeyAction::Applied || outcome.action == RKeyAction::Cleared) {
            show_original = false;
            style_toggled = true;
          } else if (outcome.action == RKeyAction::Toggled) {
            show_original = !show_original;
            style_toggled = true;
          }
        }
      }
      prefetch.set_current(images, current_id);
    }

    // 退出前显式删掉最后一帧的 placement——AltScreen 切回主屏幕缓冲区、
    // 甚至用户手动跑 `clear`,都清不掉 Kitty 协议画出来的图片,那是叠加在
    // 文字网格之上的独立层,只有协议自己的 delete 命令能清。
    pzt::cli::kitty::clear_placement(STDOUT_FILENO, mode, kImageId);
  }  // AltScreen/CbreakMode 析构,自动还原终端设置

  if (latency_count > 0) {
    std::sort(latency_samples.begin(), latency_samples.end());
    std::size_t p95_index = std::min(latency_samples.size() - 1,
                                      static_cast<std::size_t>(latency_samples.size() * 0.95));
    std::fprintf(stderr, "[pzt open] key-to-render summary: n=%zu avg=%.2fms p95=%.2fms max=%.2fms\n",
                 latency_count, latency_sum_ms / static_cast<double>(latency_count),
                 latency_samples[p95_index], latency_max_ms);
  }

  std::fprintf(stderr, "%s", pzt::cli::i18n::msg_browse_exited().c_str());
  return 0;
}

}  // namespace pzt::cli::commands
