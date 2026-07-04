#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "cli/kitty/kitty.h"

using pzt::cli::kitty::base64_encode;
using pzt::cli::kitty::fit_within;
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
