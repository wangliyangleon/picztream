#include <doctest.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

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

// F-22：三个控制台/文本纯函数从 browse.cpp 匿名空间抽到这里,补上此前因位置
// (编进可执行文件的 TU)而缺失的单元测试。take_scope_token 的引号解析是 E2E
// 修过的高危区,重点覆盖引号闭合/未闭合边界。

TEST_CASE("split_console_command splits on first whitespace, tolerating extra spaces") {
  CHECK(split_console_command("/cmd") == std::pair<std::string, std::string>{"cmd", ""});
  CHECK(split_console_command("/cmd a b") == std::pair<std::string, std::string>{"cmd", "a b"});
  // 命令名与参数间多个空格,参数不带前导空格
  CHECK(split_console_command("/cmd    a b") == std::pair<std::string, std::string>{"cmd", "a b"});
  // 只有命令名 + 尾随空格 -> 参数为空
  CHECK(split_console_command("/cmd   ") == std::pair<std::string, std::string>{"cmd", ""});
}

TEST_CASE("take_scope_token keeps a quoted #\"...\" tag as one token") {
  // 引号内的空格不算分界,整体成一个 token(保留 # 和引号)
  CHECK(take_scope_token("#\"foo bar\" rest") ==
        std::pair<std::string, std::string>{"#\"foo bar\"", "rest"});
  // CJK 带空格标签名同样整体成 token
  CHECK(take_scope_token("#\"标签 名\" 其余") ==
        std::pair<std::string, std::string>{"#\"标签 名\"", "其余"});
  // 引号后无剩余 -> rest 为空
  CHECK(take_scope_token("#\"foo bar\"") ==
        std::pair<std::string, std::string>{"#\"foo bar\"", ""});
}

TEST_CASE("take_scope_token degrades to space-split on unclosed quote / plain token") {
  // 未闭合引号:放弃引号语义,退化成按空格切
  CHECK(take_scope_token("#\"foo bar") == std::pair<std::string, std::string>{"#\"foo", "bar"});
  // 普通 token 按第一个空白切
  CHECK(take_scope_token("reject rest") == std::pair<std::string, std::string>{"reject", "rest"});
  // 前导多空格被跳过
  CHECK(take_scope_token("   reject rest") == std::pair<std::string, std::string>{"reject", "rest"});
  // 全空白 -> 两个空串
  CHECK(take_scope_token("   ") == std::pair<std::string, std::string>{"", ""});
}

TEST_CASE("wrap_tokens breaks only between tokens, counting CJK width as 2") {
  // ASCII:"aa bb cc" 宽 8,max 5 -> "aa bb"(宽 5)超一点就换,实际 "aa"+"bb"=5 放得下,加 cc 超 -> 换行
  auto ascii = wrap_tokens({"aa", "bb", "cc"}, 5);
  CHECK(ascii == std::vector<std::string>{"aa bb", "cc"});

  // CJK 每字 2 列:两个两字标签 "红叶"(4)+" "+"黄昏"(4)=9 > 6 -> 各占一行
  auto cjk = wrap_tokens({"红叶", "黄昏"}, 6);
  CHECK(cjk == std::vector<std::string>{"红叶", "黄昏"});

  // 单个 token 比 max_width 还宽 -> 独占一行(允许超出)
  auto oversized = wrap_tokens({"toolongtoken", "x"}, 4);
  CHECK(oversized == std::vector<std::string>{"toolongtoken", "x"});

  // 空输入 -> 至少一行空串
  CHECK(wrap_tokens({}, 10) == std::vector<std::string>{""});
}
