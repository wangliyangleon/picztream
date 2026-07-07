#include <doctest.h>

#include <cstdlib>
#include <string>

#include "cli/text/text.h"

using namespace pzt::cli::text;

// 显示宽度是历史上真出过 bug 的地方(6.4.3 banner 文案变长后暴露的截断
// bug),这里用真实 UTF-8 中文串覆盖 ASCII/CJK/混排的宽度、截断、补齐。

TEST_CASE("is_wide_codepoint separates CJK/fullwidth from ASCII/Latin") {
  CHECK(is_wide_codepoint(0x4E2D));   // 中
  CHECK(is_wide_codepoint(0xFF01));   // 全角感叹号 ！
  CHECK(is_wide_codepoint(0xAC00));   // 谚文音节
  CHECK_FALSE(is_wide_codepoint('a'));
  CHECK_FALSE(is_wide_codepoint(' '));
  CHECK_FALSE(is_wide_codepoint(0x00E9));  // é 拉丁扩展,窄
}

TEST_CASE("decode_utf8_at returns codepoint and byte length") {
  std::string ascii = "a";
  CHECK(decode_utf8_at(ascii, 0) == std::pair<char32_t, int>{0x61, 1});

  std::string cjk = "中";  // U+4E2D, UTF-8 = E4 B8 AD
  CHECK(decode_utf8_at(cjk, 0) == std::pair<char32_t, int>{0x4E2D, 3});
}

TEST_CASE("decode_utf8_at treats a truncated/invalid sequence as one byte") {
  std::string truncated;
  truncated.push_back(static_cast<char>(0xE4));  // 三字节前导,但后面没有续字节
  CHECK(decode_utf8_at(truncated, 0) == std::pair<char32_t, int>{0xE4, 1});
}

TEST_CASE("utf8_continuation_bytes reports trailing byte count from a lead byte") {
  CHECK(utf8_continuation_bytes('a') == 0);
  CHECK(utf8_continuation_bytes(0xC3) == 1);  // 2 字节序列前导
  CHECK(utf8_continuation_bytes(0xE4) == 2);  // 3 字节序列前导(中)
  CHECK(utf8_continuation_bytes(0xF0) == 3);  // 4 字节序列前导
  CHECK(utf8_continuation_bytes(0x80) == 0);  // 裸续字节,非法前导,按 0
}

TEST_CASE("display_width counts CJK as two columns") {
  CHECK(display_width("") == 0);
  CHECK(display_width("abc") == 3);
  CHECK(display_width("中文") == 4);     // 两个宽字符
  CHECK(display_width("中a文") == 5);    // 2 + 1 + 2
}

TEST_CASE("truncate_text cuts by display width without splitting a wide char") {
  CHECK(truncate_text("abcd", 2) == "ab");
  CHECK(truncate_text("中文", 3) == "中");   // 加"文"会到 4 列,超了,停在"中"
  CHECK(truncate_text("中文", 4) == "中文");  // 正好放得下
  CHECK(truncate_text("abc", 10) == "abc");   // 宽度够,原样返回
}

TEST_CASE("pad_to fills spaces to a fixed display width") {
  CHECK(pad_to("ab", 5) == "ab   ");   // 补 3 个空格
  CHECK(pad_to("中", 5) == "中   ");   // 宽度 2 + 3 空格 = 5
  CHECK(pad_to("中文", 3) == "中 ");   // 先截断到"中"(宽 2),再补 1 空格
  CHECK(pad_to("abc", 3) == "abc");    // 正好,不补不截
}

TEST_CASE("format_size scales through B/KB/MB") {
  CHECK(format_size(0) == "0.0B");
  CHECK(format_size(512) == "512.0B");
  CHECK(format_size(1024) == "1.0KB");
  CHECK(format_size(1536) == "1.5KB");
  CHECK(format_size(1048576) == "1.0MB");
}

TEST_CASE("expand_home_path expands a leading ~ only") {
  setenv("HOME", "/home/pzt", 1);
  CHECK(expand_home_path("~") == "/home/pzt");
  CHECK(expand_home_path("~/photos") == "/home/pzt/photos");
  CHECK(expand_home_path("/abs/path") == "/abs/path");  // 绝对路径原样
  CHECK(expand_home_path("relative") == "relative");    // 相对路径原样
  CHECK(expand_home_path("~user") == "~user");          // 不处理 ~user 形式
}
