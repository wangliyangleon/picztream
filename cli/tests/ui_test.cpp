#include <doctest.h>

#include <cstddef>
#include <string>

#include "cli/ui/ui.h"

using namespace pzt::cli::ui;

// issue #19：向导要在"当前输入为空时按 Backspace"这一种情况上回退到上一
// 个字段，而这个判断原来埋在 read_line_edit_step 里(直接读 stdin,没有
// tty 就测不了)。把退格这一步抽成纯函数之后，"空 buffer" 与"删一个完整
// UTF-8 码点"两种语义都能不碰终端地测。
TEST_CASE("apply_backspace: 空 buffer 上的退格报告出来,交给调用方决定语义") {
  std::string buffer;
  std::size_t cursor = 0;

  CHECK(apply_backspace(buffer, cursor) == true);
  CHECK(buffer.empty());
  CHECK(cursor == 0);
}

TEST_CASE("apply_backspace: 非空 buffer 删掉光标前一个字符,不报告为空") {
  std::string buffer = "abc";
  std::size_t cursor = 3;

  CHECK(apply_backspace(buffer, cursor) == false);
  CHECK(buffer == "ab");
  CHECK(cursor == 2);
}

TEST_CASE("apply_backspace: 多字节 UTF-8 整个码点一起删,不是只删一个字节") {
  std::string buffer = "a中";  // 1 + 3 字节
  std::size_t cursor = buffer.size();

  CHECK(apply_backspace(buffer, cursor) == false);
  CHECK(buffer == "a");
  CHECK(cursor == 1);
}

TEST_CASE("apply_backspace: 光标在非空 buffer 的开头时无事发生,也不算空 buffer") {
  // 这一条区分了"没东西可删"的两种成因:buffer 本身是空的(向导要据此回
  // 退),和光标停在开头、后面还有内容(既有语义:无事发生,不该被当成回退)。
  std::string buffer = "abc";
  std::size_t cursor = 0;

  CHECK(apply_backspace(buffer, cursor) == false);
  CHECK(buffer == "abc");
  CHECK(cursor == 0);
}

TEST_CASE("apply_backspace: 光标在中间时删的是光标前那个字符,后半段留着") {
  std::string buffer = "abc";
  std::size_t cursor = 2;

  CHECK(apply_backspace(buffer, cursor) == false);
  CHECK(buffer == "ac");
  CHECK(cursor == 1);
}
