#pragma once

#include <string>

#include "core/decode/decode.h"
#include "core/result.h"

// Kitty 图形协议渲染组件。见 docs/M0_Eng_Design.md increment 6.2。复用
// spikes/kitty_latency_probe/ 里验证过的 t=t(临时文件)传输介质与 Tmux DCS
// passthrough 包装,包成 cli 侧可复用的组件。
//
// 终端渲染细节(是否处于 Tmux、要不要包 passthrough、allow-passthrough 是
// 否开启)只应该出现在这里,不能下沉进 core(见 docs/M0_Eng_Design.md
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

// 启动时探测一次的运行环境。独立 Ghostty 窗口(不在 Tmux 内)下
// passthrough_ok 恒为 true——不需要 passthrough 包装,也就无所谓这个开关。
struct TerminalMode {
  bool inside_tmux = false;
  bool passthrough_ok = true;
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
pzt::core::Result<void, RenderError> render_rgba_via_tmpfile(
    int fd, const TerminalMode& mode, const pzt::core::decode::DecodedImage& img, int image_id,
    const std::string& tmp_path);

}  // namespace pzt::cli::kitty
