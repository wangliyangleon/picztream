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

// ---------------------------------------------------------------------------
// issue #20:向导每前进一步就重渲染一次预览。这里测的是"什么时候该重渲染、
// 那一刻手上的字段值是什么",预览本身怎么画是 browse.cpp 的事,不在这里。
// ---------------------------------------------------------------------------

TEST_CASE("run_field_wizard: 问第一个字段之前先交出一次全空的初始状态") {
  // 真机反馈:不先画一次的话,用户填第一个字段时屏幕上还是这张图原来的风
  // 格,没有参照物;而且第一格填 0(=不调整)按 Enter 反而会突然重渲染一次,
  // 看起来像是"输入 0 产生了效果"——实际变的是从旧风格切到了预设的底子。
  ScriptedReader reader{{cancel()}};
  std::vector<std::vector<std::string>> seen;

  run_field_wizard(
      3, [&](std::size_t i, const std::string& cur) { return reader(i, cur); },
      [&](const std::vector<std::string>& v) { seen.push_back(v); });

  // 一个字段都还没问,就已经通知过一次,内容是全空(= 预设的中性状态)。
  REQUIRE(seen.size() == 1);
  CHECK(seen[0] == std::vector<std::string>{"", "", ""});
  CHECK(reader.asked.size() == 1);  // 通知发生在第一次读字段之前
}

TEST_CASE("run_field_wizard: 每次提交都把当前全部字段值交出去一次") {
  ScriptedReader reader{{submit("10"), submit("20")}};
  std::vector<std::vector<std::string>> seen;

  auto values = run_field_wizard(
      2, [&](std::size_t i, const std::string& cur) { return reader(i, cur); },
      [&](const std::vector<std::string>& v) { seen.push_back(v); });

  REQUIRE(values.has_value());
  // 每一次都是"当前已知的完整组合",没填的字段是空串而不是缺位——预览要
  // 的正是这个:未填字段按 0 参与渲染。
  REQUIRE(seen.size() == 3);
  CHECK(seen[0] == std::vector<std::string>{"", ""});  // 进门那一次
  CHECK(seen[1] == std::vector<std::string>{"10", ""});
  CHECK(seen[2] == std::vector<std::string>{"10", "20"});
}

TEST_CASE("run_field_wizard: 回退不改变任何字段值,因此不额外通知一次") {
  // 回退只挪 index、不动 values(见 run_field_wizard 的实现),所以回退之后
  // 屏幕上那一帧预览已经就是"那一步对应的参数组合",再渲染一次是白花
  // 10-22ms 画出一模一样的东西。这条测试钉住的是"不重复通知",而不是"回退
  // 后预览不更新"——后者由 values 不变这个事实保证。
  ScriptedReader reader{{submit("10"), submit("20"), back(), submit("22"), cancel()}};
  std::vector<std::vector<std::string>> seen;

  auto values = run_field_wizard(
      3, [&](std::size_t i, const std::string& cur) { return reader(i, cur); },
      [&](const std::vector<std::string>& v) { seen.push_back(v); });

  CHECK(!values.has_value());  // 末尾取消,这条测试只看通知次数与内容
  REQUIRE(seen.size() == 4);                                   // 进门 1 次 + 3 次提交,回退不占
  CHECK(seen[2] == std::vector<std::string>{"10", "20", ""});
  CHECK(seen[3] == std::vector<std::string>{"10", "22", ""});  // 回退后重填,值变了才通知
}

TEST_CASE("run_field_wizard: 取消不通知(那一步没有产生新的参数组合)") {
  ScriptedReader reader{{submit("10"), cancel()}};
  std::vector<std::vector<std::string>> seen;

  auto values = run_field_wizard(
      2, [&](std::size_t i, const std::string& cur) { return reader(i, cur); },
      [&](const std::vector<std::string>& v) { seen.push_back(v); });

  CHECK(!values.has_value());
  CHECK(seen.size() == 2);  // 进门 1 次 + 那一次提交,取消本身不通知
}

TEST_CASE("run_field_wizard: 不传回调时照常工作") {
  // 回调是可选参数,既有调用点(以及上面那一批测试)一个字都不用改。
  ScriptedReader reader{{submit("10")}};

  auto values = run_field_wizard(1, [&](std::size_t i, const std::string& cur) { return reader(i, cur); });

  REQUIRE(values.has_value());
  CHECK(*values == std::vector<std::string>{"10"});
}

// ---------------------------------------------------------------------------
// issue #20:字段文本 -> VersionParams 的映射。它是预览与最终落库共用的唯
// 一一份,两边因此不可能算出两组不同的值——"照着预览调出来的参数,存下来
// 还是那张图"这条要求塌在这里就没救了,所以单独钉住。
// ---------------------------------------------------------------------------

TEST_CASE("params_from_wizard_fields: 8 个数值按向导顺序落到对应的参数上") {
  // 顺序即向导顺序:高光/暗光/白平衡红/白平衡蓝/对比度/饱和度/黑色/白色,
  // 第 9 格是名字,不参与参数。
  auto params = params_from_wizard_fields({"1", "2", "3", "4", "5", "6", "7", "8", "我的风格"});

  CHECK(params.highlights == doctest::Approx(1));
  CHECK(params.shadows == doctest::Approx(2));
  CHECK(params.wb_shift_r == doctest::Approx(3));
  CHECK(params.wb_shift_b == doctest::Approx(4));
  CHECK(params.contrast == doctest::Approx(5));
  CHECK(params.saturation == doctest::Approx(6));
  CHECK(params.blacks == doctest::Approx(7));
  CHECK(params.whites == doctest::Approx(8));
}

TEST_CASE("params_from_wizard_fields: 空串与解析不出数字的内容都当 0") {
  // "静默归零"这条既有哲学(见 handle_r_create_flow)在预览这条路上也必须
  // 成立:填了 abc 按 Enter,预览立刻体现"这一格是 0",不弹错、不阻塞。
  auto params = params_from_wizard_fields({"", "abc", "12abc", "-5.5", "", "", "", ""});

  CHECK(params.highlights == doctest::Approx(0));
  CHECK(params.shadows == doctest::Approx(0));
  CHECK(params.wb_shift_r == doctest::Approx(0));  // 尾部有残渣,不算部分解析成功
  CHECK(params.wb_shift_b == doctest::Approx(-5.5));
}

TEST_CASE("params_from_wizard_fields: 字段还没填完时,未填的按 0 参与") {
  // 向导走到第 3 格时手上就是这样一份 values,预览要的正是它。
  auto params = params_from_wizard_fields({"30", "-20", "", "", "", "", "", "", ""});

  CHECK(params.highlights == doctest::Approx(30));
  CHECK(params.shadows == doctest::Approx(-20));
  CHECK(params.contrast == doctest::Approx(0));
  CHECK(params.whites == doctest::Approx(0));
}

TEST_CASE("params_from_wizard_fields: 传进来的字段数不足 8 个也不越界") {
  // 防御性:字段表的长度是 recipe_menu.cpp 里的一个常量,哪天有人动了它,
  // 这里读越界会是段错误而不是编译错误。
  auto params = params_from_wizard_fields({"7"});

  CHECK(params.highlights == doctest::Approx(7));
  CHECK(params.whites == doctest::Approx(0));
}
