#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "cli/kitty/kitty.h"

using pzt::cli::kitty::base64_encode;
using pzt::cli::kitty::fit_within;
using pzt::cli::kitty::kitty_support_likely;
using pzt::cli::kitty::parse_allow_passthrough;
using pzt::cli::kitty::RenderError;
using pzt::cli::kitty::render_rgba_via_tmpfile;
using pzt::cli::kitty::TerminalMode;
using pzt::cli::kitty::tmux_wrap;

TEST_CASE("fit_within scales a landscape image down to fit a box, preserving aspect ratio") {
  // 2000x1000(2:1)塞进 800x800 的框:宽先撞到边界,800x400。
  auto r = fit_within(2000, 1000, 800, 800);
  CHECK(r.width == 800);
  CHECK(r.height == 400);
}

TEST_CASE("fit_within scales a portrait image down to fit a box, preserving aspect ratio") {
  // 1000x2000(1:2)塞进 800x800 的框:高先撞到边界,400x800。
  auto r = fit_within(1000, 2000, 800, 800);
  CHECK(r.width == 400);
  CHECK(r.height == 800);
}

TEST_CASE("fit_within leaves an image that already fits exactly unchanged") {
  auto r = fit_within(400, 300, 400, 300);
  CHECK(r.width == 400);
  CHECK(r.height == 300);
}

TEST_CASE("fit_within handles extreme aspect ratios without distortion") {
  // 一张极端的全景图(10000x100)塞进一个矮框,不应该被拉伸变形。
  auto r = fit_within(10000, 100, 1000, 500);
  CHECK(r.width == 1000);
  CHECK(r.height == 10);  // 保持 100:1 的原始比例
}

TEST_CASE("fit_within returns {0,0} for non-positive inputs") {
  CHECK(fit_within(0, 100, 800, 800).width == 0);
  CHECK(fit_within(100, 0, 800, 800).width == 0);
  CHECK(fit_within(100, 100, 0, 800).width == 0);
  CHECK(fit_within(100, 100, 800, 0).width == 0);
}

TEST_CASE("tmux_wrap wraps in DCS passthrough and doubles embedded ESC bytes") {
  std::string raw = "\x1b_Gfoo\x1b\\";
  std::string wrapped = tmux_wrap(raw);
  CHECK(wrapped.rfind("\x1bPtmux;", 0) == 0);
  CHECK(wrapped.substr(wrapped.size() - 2) == "\x1b\\");
  // Every ESC byte inside the raw payload must appear twice in the wrapped
  // output (tmux's passthrough escaping rule), on top of the two ESC bytes
  // introduced by the DCS wrapper itself (open + close).
  auto count_esc = [](const std::string& s) {
    return std::count(s.begin(), s.end(), '\x1b');
  };
  CHECK(count_esc(wrapped) == count_esc(raw) * 2 + 2);
}

TEST_CASE("base64_encode matches known test vectors (RFC 4648)") {
  auto encode_str = [](const std::string& s) {
    return base64_encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
  };
  CHECK(encode_str("") == "");
  CHECK(encode_str("f") == "Zg==");
  CHECK(encode_str("fo") == "Zm8=");
  CHECK(encode_str("foo") == "Zm9v");
  CHECK(encode_str("foob") == "Zm9vYg==");
  CHECK(encode_str("fooba") == "Zm9vYmE=");
  CHECK(encode_str("foobar") == "Zm9vYmFy");
}

TEST_CASE("parse_allow_passthrough only accepts the literal 'on'") {
  CHECK(parse_allow_passthrough("on") == true);
  CHECK(parse_allow_passthrough("off") == false);
  CHECK(parse_allow_passthrough("") == false);
  CHECK(parse_allow_passthrough("On") == false);  // tmux 输出恒为小写,大小写不同即视为异常
}

namespace {
// 环境变量替身:一张 name->value 的表,不碰进程真实环境(不 setenv/unsetenv,
// 测试之间零串扰)。这正是 kitty_support_likely 把 env 做成注入参数的目的。
pzt::cli::kitty::EnvLookupFn env_with(std::map<std::string, std::string> vars) {
  return [vars](std::string_view name) -> std::optional<std::string> {
    auto it = vars.find(std::string(name));
    if (it == vars.end()) return std::nullopt;
    return it->second;
  };
}
}  // namespace

TEST_CASE("kitty_support_likely accepts Ghostty outside tmux by TERM_PROGRAM or TERM") {
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", "ghostty"},
                                        {"TERM", "xterm-ghostty"}}),
                              /*inside_tmux=*/false) == true);
  // 只有其中一个也够:任一条命中即命中,不做优先级。
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", "ghostty"}}), false) == true);
  CHECK(kitty_support_likely(env_with({{"TERM", "xterm-ghostty"}}), false) == true);
}

TEST_CASE("kitty_support_likely accepts kitty itself outside tmux") {
  CHECK(kitty_support_likely(env_with({{"TERM", "xterm-kitty"}}), false) == true);
  CHECK(kitty_support_likely(env_with({{"KITTY_WINDOW_ID", "1"}}), false) == true);
}

TEST_CASE("kitty_support_likely matches TERM by prefix, not exact equality") {
  // terminfo 名字带后缀的变体(xterm-ghostty-256color 之类)仍是同一个终端。
  CHECK(kitty_support_likely(env_with({{"TERM", "xterm-ghostty-256color"}}), false) == true);
  CHECK(kitty_support_likely(env_with({{"TERM", "xterm-kitty-direct"}}), false) == true);
}

TEST_CASE("kitty_support_likely rejects a terminal outside the whitelist") {
  // iTerm2:write() 会成功、终端把 APC 序列丢掉,正是本增量要救的静默失效场景。
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", "iTerm.app"},
                                        {"TERM", "xterm-256color"}}),
                              false) == false);
  // Terminal.app
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", "Apple_Terminal"},
                                        {"TERM", "xterm-256color"}}),
                              false) == false);
  // 什么都没有的裸环境(cron/CI/裸 TTY)。
  CHECK(kitty_support_likely(env_with({}), false) == false);
}

TEST_CASE("kitty_support_likely treats an empty env var as absent") {
  // TERM_PROGRAM= (设了但为空)不该被当成命中,否则空值反而比没设更宽松。
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", ""}, {"TERM", ""}}), false) == false);
  CHECK(kitty_support_likely(env_with({{"GHOSTTY_BIN_DIR", ""}}), true) == false);
}

TEST_CASE("kitty_support_likely inside tmux relies on GHOSTTY_* only") {
  // 实测本机 pane 内的真实取值:Ghostty 的身份被 tmux 擦掉了(TERM_PROGRAM
  // 变成 tmux、TERM 变成 screen-256color),唯一残留的信号是 GHOSTTY_*。
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", "tmux"},
                                        {"TERM", "screen-256color"},
                                        {"GHOSTTY_BIN_DIR", "/Applications/Ghostty.app/Contents/MacOS"}}),
                              /*inside_tmux=*/true) == true);
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", "tmux"},
                                        {"GHOSTTY_RESOURCES_DIR", "/Applications/Ghostty.app/..."}}),
                              true) == true);
  CHECK(kitty_support_likely(env_with({{"KITTY_WINDOW_ID", "1"}}), true) == true);
}

TEST_CASE("kitty_support_likely inside tmux ignores TERM and TERM_PROGRAM entirely") {
  // 这条锁住决策二:tmux 内这两个变量的值只可能来自 tmux 自己,看它们会稳
  // 定误判。即便 TERM_PROGRAM 恰好是 ghostty(比如用户手动 export 过),没有
  // GHOSTTY_* 就仍然判不命中 - 依据必须是那条真正残留下来的信号。
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", "ghostty"},
                                        {"TERM", "xterm-ghostty"}}),
                              /*inside_tmux=*/true) == false);
  CHECK(kitty_support_likely(env_with({{"TERM_PROGRAM", "tmux"},
                                        {"TERM", "screen-256color"}}),
                              true) == false);
}

TEST_CASE("TerminalMode defaults to not warning") {
  // 默认 true(静默)是刻意的:非交互调用方(pzt render)和单测不该因为没探测
  // 就收到提示。跟 passthrough_ok 默认 true 是同一个语义。
  TerminalMode mode;
  CHECK(mode.kitty_support_likely == true);
}

TEST_CASE("render_rgba_via_tmpfile refuses to send when in tmux without passthrough") {
  pzt::core::decode::DecodedImage img;
  img.width = 1;
  img.height = 1;
  img.rgba = {255, 0, 0, 255};

  TerminalMode mode;
  mode.inside_tmux = true;
  mode.passthrough_ok = false;

  auto result = render_rgba_via_tmpfile(STDOUT_FILENO, mode, img, /*image_id=*/1,
                                         "/tmp/pzt_kitty_test_should_not_be_created.rgba");
  REQUIRE(!result.ok());
  CHECK(result.error() == RenderError::PassthroughDisabled);
}

TEST_CASE("render_rgba_via_tmpfile writes the tmp file and control sequence when allowed") {
  pzt::core::decode::DecodedImage img;
  img.width = 2;
  img.height = 1;
  img.rgba = {255, 0, 0, 255, 0, 255, 0, 255};

  TerminalMode mode;
  mode.inside_tmux = false;  // 独立 Ghostty 窗口路径,不需要 passthrough 检测

  std::string tmp_path = "/tmp/pzt_kitty_test_write.rgba";
  // fd 0 (stdin) 只是拿来当一个必然存在、可写的 fd 验证控制序列写入不出错,
  // 不代表真的把图片发到了 stdin - 单元测试环境没有真实终端可验证渲染
  // 效果,那部分留给 cli/render 调试命令做端到端验证。
  int fd = open("/dev/null", O_WRONLY);
  REQUIRE(fd >= 0);
  auto result = render_rgba_via_tmpfile(fd, mode, img, /*image_id=*/2, tmp_path);
  close(fd);
  CHECK(result.ok());

  std::ifstream f(tmp_path, std::ios::binary);
  REQUIRE(f.good());
  std::vector<unsigned char> written((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
  CHECK(written == img.rgba);
  std::remove(tmp_path.c_str());
}

TEST_CASE("render_rgba_via_tmpfile removes the tmp file when writing the control sequence fails") {
  pzt::core::decode::DecodedImage img;
  img.width = 2;
  img.height = 1;
  img.rgba = {255, 0, 0, 255, 0, 255, 0, 255};

  TerminalMode mode;
  mode.inside_tmux = false;  // 独立 Ghostty 窗口路径,不需要 passthrough 检测

  std::string tmp_path = "/tmp/pzt_kitty_test_write_failed.rgba";
  std::remove(tmp_path.c_str());  // 保证从干净状态开始

  // 已关闭的 fd:临时文件照写,但 write() 控制序列必然 EBADF 失败,复现
  // WriteFailed 分支——终端没收到序列、不会去读也不会删临时文件。
  int fd = open("/dev/null", O_WRONLY);
  REQUIRE(fd >= 0);
  close(fd);
  auto result = render_rgba_via_tmpfile(fd, mode, img, /*image_id=*/3, tmp_path);
  REQUIRE(!result.ok());
  CHECK(result.error() == RenderError::WriteFailed);

  // 失败路径必须显式清理临时文件,不能留孤儿。
  std::ifstream leftover(tmp_path, std::ios::binary);
  CHECK_FALSE(leftover.good());
  std::remove(tmp_path.c_str());
}
