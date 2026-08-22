#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "cli/kitty/kitty.h"
#include "core/decode/decode.h"
#include "core/result.h"

// 两图并排比较界面:给两张已解码的图和一行进度描述,画出来,等一次按键,
// 得出「选左」「选右」「放弃」三者之一。
//
// 这一层是 pzt open 里的一个全屏子界面,不是菜单:cli/menu 那几个交互按约
// 定不依赖 cli/kitty(它们只收回调、把画图这件事留给调用方),而这个界面的
// 全部内容就是两张图,画图是它自己的职责,所以单开一层。
//
// 界面上只有两张图和进度行,不画文件名、拍摄时间、已有标签 - 比较应当基于
// 画面本身,元数据会把判断锚到与画质无关的东西上。进度行的文字由调用方
// 给,这一层只负责把它渲染在固定位置,不定义计数怎么数。
namespace pzt::cli::compare {

// 比较界面自己用的两个 Kitty image id。同一个终端会话里 image id 是全局
// 的,跟浏览界面那张图的 id 撞了就会互相覆盖图像数据,所以在这里把它们占
// 住、写明白。
inline constexpr int kLeftImageId = 2;
inline constexpr int kRightImageId = 3;

// 一次比较的结论。
enum class CompareChoice { Left, Right, Abandon };

// 一个按键的语义。把"这个字节意味着什么"从终端 io 里剥出来,循环的分支结
// 构才能不带 tty 地测。
//
// h/l 选左右与浏览态「上一张/下一张」同向,借的是已经练出来的肌肉记忆。
// Esc 只是**请求**放弃,不直接放弃:它离 h/l 很近,而误触的代价是此前所有
// 按键全部作废,所以要走一次二次确认。其余按键静默忽略,不透传给外层的浏
// 览主循环 - 这个界面期间按 q 不应该退出程序。
enum class CompareKey { PickLeft, PickRight, RequestAbandon, Ignored };
CompareKey classify_key(char c);

// 读一个按键;nullopt 表示 stdin 到头或出错。
//
// 这里不能沿用 ui::read_one_byte 把 EOF 折叠成 Esc 的做法:那种折叠对"读
// 到取消键就结束"的菜单够用,但这个循环读到 Esc 之后还要再读一次确认,EOF
// 之下两次读都返回 Esc、确认永远不是 y,循环就空转了。EOF 意味着没有人可
// 以确认,直接按放弃处理(放弃不写任何东西,是安全的那一侧)。
using ReadKeyFn = std::function<std::optional<char>()>;

// 把当前这一场重画一遍。进入循环时调用一次;二次确认被取消之后再调用一次
// (确认提示盖掉了进度行,回到这一场得把画面恢复回来)。
using DrawPairFn = std::function<void()>;

// 问一次"真的放弃吗",true 表示放弃。实现方要把 EOF 也算成 true,理由同
// ReadKeyFn。
using ConfirmAbandonFn = std::function<bool()>;

// 一场比较的按键循环。忽略的按键不触发重画 - 画面本来就没变,重画一次只
// 会让终端多收一遍两张图的像素。
CompareChoice run_compare_loop(const DrawPairFn& draw_pair, const ReadKeyFn& read_key,
                                const ConfirmAbandonFn& confirm_abandon);

// 算布局要用到的终端几何。cell 的像素尺寸是必需的:Kitty 协议的 c=/r= 以
// cell 计,而保持长宽比这件事只能在像素上算。
//
// cell_px_w/cell_px_h 为 0 表示终端没上报像素尺寸(部分终端会把
// ws_xpixel/ws_ypixel 报成 0),compute_layout 会换一组常见比例兜底。
struct TerminalGeometry {
  int cols = 0;
  int rows = 0;
  int cell_px_w = 0;
  int cell_px_h = 0;
};

// 一张图画在哪、画多大。row/col 是 1-based 的落点(直接喂给光标定位),
// cols/rows 是传给 Kitty 协议 c=/r= 的 cell 数,px_w/px_h 是发给终端之前先
// 在本地降采样到的目标像素尺寸。
//
// 三组数都要:只给 cell 数,终端会拿到原始分辨率的整张图自己缩,那是真机实
// 测出来的切图卡顿来源;只给像素尺寸,终端不知道该占几个 cell。
//
// cols/rows 为 0 表示这一栏画不下(终端太小,或者传进来的图片尺寸非正)。
struct PanePlacement {
  int row = 0;
  int col = 0;
  int cols = 0;
  int rows = 0;
  int px_w = 0;
  int px_h = 0;
};

// 整屏几何:左右两栏 + 中间一条竖直分隔线 + 底部一行进度。
//
// 分隔线不是装饰:同一场景的两张照片并排时,只靠几列黑边很难看出一块暗部
// 属于左边还是右边,而这个界面上唯一要做的判断就是"哪一张更好"。
// divider_col 为 0 表示终端窄到栏间距都让掉了,这一帧没有分隔线。
struct CompareLayout {
  PanePlacement left;
  PanePlacement right;
  int divider_col = 0;
  int divider_top_row = 0;
  int divider_rows = 0;
  int progress_row = 0;
  int progress_col = 0;
  int progress_cols = 0;
};

// 纯函数:给定终端几何与两张图的原始尺寸,算出两个互不重叠、都不越界、各
// 自保持自己长宽比的落点与尺寸。
//
// 比较界面铺满终端宽度,不套用浏览界面那个居中 70% 的框:那个框是为了给右
// 侧信息栏留位置,而这里屏幕上除了两张图什么都没有,少给的每一列都是直接
// 损失掉的判断依据。
//
// 两栏各自独立跑一次 fit_within,而不是先算出一个公共尺寸再套给两张 - 一
// 横一竖配在一起时,公共尺寸必然按更受限的那一维取,横图会白白缩小。
//
// 终端小到放不下时逐级让步:先去掉留白,再去掉栏间距,最后返回尺寸为 0 的
// pane 表示"这里画不下",由调用方决定怎么办。
CompareLayout compute_layout(const TerminalGeometry& term, int left_w, int left_h, int right_w,
                              int right_h);

// 把一帧发到 fd:先清掉上一帧两张图的 placement,再画分隔线、画左、画右,
// 最后写进度行。清 placement 必须在前 - Kitty 协议里"更新一个 id 的图像数据"和"清除
// 这个 id 已经画出来的 placement"是两件事,不清的话上一对不会自动消失,新
// 旧四张会叠在一起。
//
// fd 是参数而不是写死 STDOUT,这样单测能把整帧字节流收进一根管道,核对两
// 条清除序列确实排在两条传输序列前面。frame 是调用方持有的帧计数,只用来
// 让每帧的临时文件路径不撞车。
//
// 两张图用两个不同的 image id,否则第二张会覆盖掉第一张的图像数据。
pzt::core::Result<void, kitty::RenderError> draw_pair_frame(
    int fd, const kitty::TerminalMode& mode, const CompareLayout& layout,
    const pzt::core::decode::DecodedImage& left, const pzt::core::decode::DecodedImage& right,
    int left_image_id, int right_image_id, const std::string& progress_line, std::size_t& frame);

// 一整轮比较期间的界面。构造时清屏接管画面,析构时清掉两张图的
// placement - 之后由调用方整屏重绘回浏览界面。做成 RAII 是因为"离开时不留
// 残影"这件事不能指望每个调用点自己记得,尤其是中途放弃那条路径。
//
// 不碰信号处置:这个界面期间的 Ctrl-C 仍然是既有的那条路径(还原终端后干净
// 退出 pzt)。取消动作选 Esc 而不是 Ctrl-C,是因为这个界面的全部形态就是在
// 读键,Esc 是循环里的一个普通分支,不需要借道信号。
class CompareView {
 public:
  explicit CompareView(const kitty::TerminalMode& mode);
  ~CompareView();

  CompareView(const CompareView&) = delete;
  CompareView& operator=(const CompareView&) = delete;

  // 比一场。comparisons_done 是本轮此前已经比过的次数,只用于放弃确认的文
  // 案("这些将全部作废")。
  CompareChoice compare(const pzt::core::decode::DecodedImage& left,
                        const pzt::core::decode::DecodedImage& right,
                        const std::string& progress_line, int comparisons_done);

 private:
  kitty::TerminalMode mode_;
  std::size_t frame_ = 0;
};

}  // namespace pzt::cli::compare
