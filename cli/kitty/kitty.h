#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/decode/decode.h"
#include "core/result.h"

// Kitty 图形协议渲染组件。见 docs/history/M0_Eng_Design.md increment 6.2。复用
// spikes/kitty_latency_probe/ 里验证过的 t=t(临时文件)传输介质与 Tmux DCS
// passthrough 包装,包成 cli 侧可复用的组件。
//
// 终端渲染细节(是否处于 Tmux、要不要包 passthrough、allow-passthrough 是
// 否开启)只应该出现在这里,不能下沉进 core(见 docs/history/M0_Eng_Design.md
// "对 core 设计的直接影响"一节)。
//
// 这一层只负责"已解码像素 -> 终端字节",不做 JPEG 解码(core/decode 的职
// 责)、不做预取调度(increment 6.3 的职责)。
namespace pzt::cli::kitty {

bool is_inside_tmux();

// 生成一个符合 Kitty 协议 t=t(临时文件)传输介质要求的路径。协议规定:
// 路径必须在系统真实临时目录内(不能想当然假设是 `/tmp`——macOS 的
// `$TMPDIR` 通常是 `/var/folders/.../T/`,不是 `/tmp`),文件名必须包含协
// 议要求的 "tty-graphics-protocol" 标记字符串,否则终端会以"不在临时目录
// 内"为由拒绝读取(实测过:Ghostty 返回 `EINVAL: temporary file not in
// temp dir`)。`tag` 由调用方提供,用来保证多次调用之间路径不冲突(比如
// pid、帧号)。
std::string make_tmp_path(const std::string& tag);

// Tmux DCS passthrough 包装规则:整体包一层 `\x1bPtmux;...\x1b\\`,内部每个
// ESC 字节需要再多写一次(tmux passthrough 协议本身的转义要求)。只应该在
// is_inside_tmux() 时对最终发送的转义序列调用。
std::string tmux_wrap(const std::string& raw);

std::string base64_encode(const unsigned char* data, std::size_t len);

// `tmux show-options -gqv allow-passthrough` 的输出经过 trim 后只可能是
// "on"/"off"(这个布尔选项的两个合法值)或者空字符串(选项未显式设置过,
// tmux 默认值就是 off)。抽成纯函数方便单元测试,不需要真的起一个 tmux
// 会话。
bool parse_allow_passthrough(const std::string& trimmed_output);

// 环境变量的取值函数。抽成参数注入(而不是函数内部直接 getenv)是为了让
// kitty_support_likely 能在单测里构造任意环境组合,不用 setenv/unsetenv 污
// 染进程真实环境,跟 parse_allow_passthrough 抽成纯函数是同一个动机。
// 返回 nullopt 表示该变量未设置。
using EnvLookupFn = std::function<std::optional<std::string>(std::string_view)>;

// 白名单判定:当前终端是不是"很可能"讲 Kitty 图像协议。
//
// 为什么是白名单而不是主动查询:协议本身有 `a=q` 查询,但那要求在 raw 模式
// 下带超时读 stdin,跟全键盘主循环抢同一个 fd,且与 render_rgba_via_tmpfile
// 里 `q=2`("终端永远不要回发响应")的约定直接冲突 - 而那条约定是为修一次
// 真实死循环立的。所以这里只按环境变量猜,判定结果只用来决定"要不要提示
// 一句",不用来拦人(见 docs/Env_Preflight_PRD.md 决策一)。
//
// inside_tmux 显式传入而不是内部再探一次:调用方已经算过,且单测要能独立
// 构造 tmux 内/外两种情形。tmux 内 TERM/TERM_PROGRAM 只反映 tmux 自己,这
// 个函数会跳过它们,只认 GHOSTTY_*/KITTY_WINDOW_ID。
bool kitty_support_likely(const EnvLookupFn& lookup, bool inside_tmux);

// 启动时探测一次的运行环境。独立 Ghostty 窗口(不在 Tmux 内)下
// passthrough_ok 恒为 true——不需要 passthrough 包装,也就无所谓这个开关。
struct TerminalMode {
  bool inside_tmux = false;
  bool passthrough_ok = true;
  // 白名单命中为 true。默认 true(不提示)是刻意的:非交互调用方(pzt render)
  // 和单测不该因为压根没探测就收到提示,跟 passthrough_ok 默认 true 同理。
  bool kitty_support_likely = true;
};

// 实际调用 `tmux show-options` 查询当前 allow-passthrough 设置(仅在
// is_inside_tmux() 为真时才会真的起子进程查询)。
TerminalMode detect_terminal_mode();

enum class RenderError {
  PassthroughDisabled,  // 处于 Tmux 内,但 allow-passthrough 未开启
  WriteFailed,           // 写入目标 fd 失败
};

// 用 t=t(临时文件)传输介质把一张已解码的 RGBA 图片发送到 fd 对应的终端。
// tmp_path 由调用方选择(用 make_tmp_path() 生成,不要手写)——不同调用场
// 景(一次性调试命令 vs 全键盘循环里每帧都要渲染)对临时文件的生命周期管
// 理需求不同,渲染组件本身不做假设。协议约定终端读完 `t=t` 介质的文件后
// 会自己删除,调用方不需要额外 unlink(与
// spikes/kitty_latency_probe/probe.cpp 的处理方式一致)。控制序列带
// `q=2`,让终端永远不通过 stdin 回发协议响应——调用方(全键盘循环)从 stdin
// 读的是用户按键,没有能力也不需要分辨"这是真按键"还是"这是终端在回话",
// 混在一起处理曾经导致过一次真实的死循环(终端因为渲染失败不断回发错误
// 文本,被当成按键消费,又触发下一次同样失败的渲染)。
//
// target_cols/target_rows 大于 0 时,会作为 Kitty 协议的 c=/r= 参数传下
// 去,让终端把图片缩放进正好这么多 cell 的区域——调用方(布局代码)应该先
// 用 fit_within() 算出保持长宽比的目标像素尺寸,再换算成对应的 cell 数,
// 而不是直接把面板的 cell 框硬套给 c=/r=,否则长宽比对不上时会被拉伸变
// 形。都传 0(默认)表示不做缩放约束,按原始像素尺寸显示,一次性调试命令
// (`pzt render`)用的就是这个默认路径。
pzt::core::Result<void, RenderError> render_rgba_via_tmpfile(
    int fd, const TerminalMode& mode, const pzt::core::decode::DecodedImage& img, int image_id,
    const std::string& tmp_path, int target_cols = 0, int target_rows = 0);

// 删除指定 image_id 已经画到屏幕上的所有 placement。每帧画新图之前都应该
// 先调用这个清掉上一帧,否则旧图不会自动消失(Kitty 协议里"更新一个 id 的
// 图像数据"和"清除这个 id 已经画出来的 placement"是两件不同的事)。
pzt::core::Result<void, RenderError> clear_placement(int fd, const TerminalMode& mode,
                                                       int image_id);

struct FitSize {
  int width;
  int height;
};

// 在不超出 box_w x box_h 的前提下,算出保持 image_w/image_h 原始长宽比的
// 最大尺寸(不拉伸变形)。任何一个输入 <= 0 时返回 {0, 0}。纯函数,不依赖
// 真实终端,可以直接写单元测试。
FitSize fit_within(int image_w, int image_h, int box_w, int box_h);

}  // namespace pzt::cli::kitty
