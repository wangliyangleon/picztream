#include <doctest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "cli/menu/recipe_menu.h"
#include "cli/ui/ui.h"

using namespace pzt::cli::menu;
using pzt::cli::ui::WizardLineAction;
using pzt::cli::ui::WizardLineResult;

namespace {

WizardLineResult submit(const std::string& text) { return {WizardLineAction::Submitted, text}; }
WizardLineResult back() { return {WizardLineAction::Back, ""}; }
WizardLineResult cancel() { return {WizardLineAction::Cancelled, ""}; }

// 按脚本回放一串按键结果，同时记录每次被问到的是哪个字段、回填的初值是
// 什么:"回退之后看到的是上一次为该字段提交的值"这条验收标准，断言的正是
// 这份记录。
struct ScriptedReader {
  std::vector<WizardLineResult> script;
  std::vector<std::pair<std::size_t, std::string>> asked = {};
  std::size_t next = 0;

  WizardLineResult operator()(std::size_t index, const std::string& current) {
    asked.push_back({index, current});
    REQUIRE(next < script.size());
    return script[next++];
  }
};

}  // namespace

TEST_CASE("run_field_wizard: 一路提交,按顺序收齐每个字段的值") {
  ScriptedReader reader{{submit("10"), submit("20"), submit("30")}};

  auto values = run_field_wizard(3, [&](std::size_t i, const std::string& cur) { return reader(i, cur); });

  REQUIRE(values.has_value());
  CHECK(*values == std::vector<std::string>{"10", "20", "30"});
}

TEST_CASE("run_field_wizard: 回退到上一个字段,并回填该字段上次提交的值") {
  // 字段 0 填 10、字段 1 填 20，在字段 2 上回退 → 应该回到字段 1 且看到
  // "20"，改成 99 之后继续往前走。
  ScriptedReader reader{{submit("10"), submit("20"), back(), submit("99"), submit("30")}};

  auto values = run_field_wizard(3, [&](std::size_t i, const std::string& cur) { return reader(i, cur); });

  REQUIRE(values.has_value());
  CHECK(*values == std::vector<std::string>{"10", "99", "30"});
  REQUIRE(reader.asked.size() == 5);
  CHECK(reader.asked[2].first == 2);                 // 回退发生在字段 2 上
  CHECK(reader.asked[3] == std::pair<std::size_t, std::string>{1, "20"});  // 回到字段 1,带旧值
}

TEST_CASE("run_field_wizard: 第一个字段上的回退无效,停在原地不越界") {
  ScriptedReader reader{{back(), back(), submit("10"), submit("20")}};

  auto values = run_field_wizard(2, [&](std::size_t i, const std::string& cur) { return reader(i, cur); });

  REQUIRE(values.has_value());
  CHECK(*values == std::vector<std::string>{"10", "20"});
  REQUIRE(reader.asked.size() == 4);
  CHECK(reader.asked[0].first == 0);
  CHECK(reader.asked[1].first == 0);
  CHECK(reader.asked[2].first == 0);
}

TEST_CASE("run_field_wizard: 任意字段上取消,整个流程作废") {
  ScriptedReader reader{{submit("10"), cancel()}};

  auto values = run_field_wizard(3, [&](std::size_t i, const std::string& cur) { return reader(i, cur); });

  CHECK(!values.has_value());
}

TEST_CASE("run_field_wizard: 第一个字段上直接取消") {
  ScriptedReader reader{{cancel()}};

  auto values = run_field_wizard(9, [&](std::size_t i, const std::string& cur) { return reader(i, cur); });

  CHECK(!values.has_value());
}

TEST_CASE("run_field_wizard: 连退两格再往前走,后面已填的字段原样等在那里") {
  // 用户改的是前面某一格，后面已经填好的内容不该被清空。
  ScriptedReader reader{{submit("10"), submit("20"), submit("30"), back(), back(), submit("22"),
                          submit("33"), submit("40")}};

  auto values = run_field_wizard(4, [&](std::size_t i, const std::string& cur) { return reader(i, cur); });

  REQUIRE(values.has_value());
  CHECK(*values == std::vector<std::string>{"10", "22", "33", "40"});
  REQUIRE(reader.asked.size() == 8);
  CHECK(reader.asked[4] == std::pair<std::size_t, std::string>{2, "30"});  // 第二次回退,带旧值
  CHECK(reader.asked[5] == std::pair<std::size_t, std::string>{1, "20"});
}
