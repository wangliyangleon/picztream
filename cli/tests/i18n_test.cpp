#include <doctest.h>
#include <cstdlib>
#include <string>

#include "cli/i18n/i18n.h"

using namespace pzt::cli::i18n;

TEST_CASE("i18n language initialization and switching") {
  // F-12 之后 init_lang() 会读 Settings.lang，Settings 又是从
  // XDG_CONFIG_HOME/pzt/config.json 加载的——不隔离这个环境变量的话，
  // 这条用例读的是开发者/用户真实的 config.json，一旦那份文件里写了
  // "lang" 字段（比如手动测过 F-12 之后忘记还原），就会盖过这里
  // setenv("LANG", ...) 想测的系统 LANG 兜底逻辑，测试变得依赖运行机
  // 器上的真实文件内容。指向一个确定不存在的目录，保证 Settings 全程
  // 是默认值(lang = nullopt)。
  const char* old_xdg = std::getenv("XDG_CONFIG_HOME");
  std::string old_xdg_value = old_xdg ? old_xdg : "";
  bool had_old_xdg = old_xdg != nullptr;
  setenv("XDG_CONFIG_HOME", "/nonexistent/pzt_i18n_test_isolation", 1);

  // Test fallback logic
  unsetenv("PZT_LANG");
  unsetenv("LANG");
  init_lang();
  CHECK(g_lang == Lang::zh); // Default should be zh

  // Test LANG setting
  setenv("LANG", "en_US.UTF-8", 1);
  init_lang();
  CHECK(g_lang == Lang::en);

  setenv("LANG", "zh_CN.UTF-8", 1);
  init_lang();
  CHECK(g_lang == Lang::zh);

  // Test PZT_LANG override
  setenv("PZT_LANG", "en", 1);
  init_lang();
  CHECK(g_lang == Lang::en);

  setenv("PZT_LANG", "zh", 1);
  init_lang();
  CHECK(g_lang == Lang::zh);
  
  unsetenv("PZT_LANG");
  unsetenv("LANG");

  if (had_old_xdg) {
    setenv("XDG_CONFIG_HOME", old_xdg_value.c_str(), 1);
  } else {
    unsetenv("XDG_CONFIG_HOME");
  }
}

TEST_CASE("i18n localized text strings") {
  // g_lang 是跨整个 cli_tests 可执行文件共享的全局状态,doctest 不会在用
  // 例之间重置它——测试结束前必须显式还原成默认值,不然这个用例执行顺序
  // 之后的任何东西都会意外看到 en。
  g_lang = Lang::zh;
  CHECK(!menu_lines().empty());
  CHECK(menu_lines()[0].text.find("打标签") != std::string::npos);
  CHECK(nav_bar_line1().find("上一张") != std::string::npos);
  CHECK(nav_bar_line2().find("退出") != std::string::npos);
  CHECK(info_tags_label() == "标签:");

  g_lang = Lang::en;
  CHECK(!menu_lines().empty());
  CHECK(menu_lines()[0].text.find("Tag") != std::string::npos);
  CHECK(nav_bar_line1().find("Prev") != std::string::npos);
  CHECK(nav_bar_line2().find("Quit") != std::string::npos);
  CHECK(info_tags_label() == "Tags:");

  g_lang = Lang::zh;  // 还原成默认值,不泄漏状态给其它测试用例
}

// F-11：dedup 结果文案在实际标记到重复图片时带上"按 g 9 查看"入口提
// 示；标记数为 0 时不带（范围内没有新重复组，提示了也是空列表）。
TEST_CASE("msg_dedup_result includes entry hint only when images were tagged") {
  g_lang = Lang::zh;
  auto zh_tagged = msg_dedup_result(2, 4, 0);
  CHECK(zh_tagged.find("2") != std::string::npos);
  CHECK(zh_tagged.find("4") != std::string::npos);
  CHECK(zh_tagged.find("g 9") != std::string::npos);

  auto zh_empty = msg_dedup_result(0, 0, 0);
  CHECK(zh_empty.find("g 9") == std::string::npos);

  g_lang = Lang::en;
  auto en_tagged = msg_dedup_result(2, 4, 0);
  CHECK(en_tagged.find("g 9") != std::string::npos);

  auto en_empty = msg_dedup_result(0, 0, 0);
  CHECK(en_empty.find("g 9") == std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// F-08：范围内有图片因为没有拍摄时间被跳过时,结果文案带一句提示;
// 没有跳过的常见路径不受影响,不多带一句空话。
TEST_CASE("msg_dedup_result mentions skipped-no-capture-time count only when nonzero") {
  g_lang = Lang::zh;
  auto zh_skipped = msg_dedup_result(1, 2, 3);
  CHECK(zh_skipped.find("3") != std::string::npos);
  CHECK(zh_skipped.find("拍摄时间") != std::string::npos);

  auto zh_none = msg_dedup_result(1, 2, 0);
  CHECK(zh_none.find("拍摄时间") == std::string::npos);

  g_lang = Lang::en;
  auto en_skipped = msg_dedup_result(1, 2, 3);
  CHECK(en_skipped.find("capture time") != std::string::npos);

  auto en_none = msg_dedup_result(1, 2, 0);
  CHECK(en_none.find("capture time") == std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// F-09：控制台二级筛选四个关键字各自映射到对应的中文标签,英文路径原
// 样透出关键字(不额外维护一份英文词表)。
TEST_CASE("info_console_filter_label maps each keyword to the agreed vocabulary") {
  g_lang = Lang::zh;
  CHECK(info_console_filter_label("unevaluated").find("未评估") != std::string::npos);
  CHECK(info_console_filter_label("fail").find("评估不达标") != std::string::npos);
  CHECK(info_console_filter_label("reject").find("废片") != std::string::npos);
  CHECK(info_console_filter_label("dup").find("重复") != std::string::npos);

  g_lang = Lang::en;
  CHECK(info_console_filter_label("unevaluated").find("unevaluated") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// F-09：`/filter` 计算结果为空、以及非法筛选条件这两条独立文案,分别跟
// msg_filter_no_images(标签语义)和 err_console_invalid_scope(范围语
// 法)区分开,不复用。
TEST_CASE("msg_console_filter_no_images and err_console_invalid_filter_criterion follow language") {
  g_lang = Lang::zh;
  CHECK(msg_console_filter_no_images().find("符合条件") != std::string::npos);
  CHECK(err_console_invalid_filter_criterion().find("unevaluated") != std::string::npos);

  g_lang = Lang::en;
  CHECK(msg_console_filter_no_images().find("match this filter") != std::string::npos);
  CHECK(err_console_invalid_filter_criterion().find("unevaluated") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// F-09：placeholder 提示要包含新命令的用法,不然用户按 `:` 之后完全不
// 知道 /filter 存在。
TEST_CASE("msg_ai_prompt_placeholder mentions /filter usage") {
  g_lang = Lang::zh;
  CHECK(msg_ai_prompt_placeholder().find("/filter") != std::string::npos);

  g_lang = Lang::en;
  CHECK(msg_ai_prompt_placeholder().find("/filter") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// /help：不带参数列出全部命令；带一个已知命令名返回该命令的详细用
// 法；带一个不认识的命令名返回 nullopt，调用方据此转去
// err_help_unknown_command。
TEST_CASE("msg_help_overview lists every command and msg_help_command covers each one") {
  g_lang = Lang::zh;
  auto overview = msg_help_overview();
  for (const char* cmd : {"/ai_eval", "/dedup", "/tasks", "/filter", "/help"}) {
    CHECK(overview.find(cmd) != std::string::npos);
  }

  for (const char* cmd : {"ai_eval", "dedup", "tasks", "filter", "help"}) {
    auto detail = msg_help_command(cmd);
    REQUIRE(detail.has_value());
    CHECK(detail->find(std::string("/") + cmd) != std::string::npos);
  }
  CHECK(!msg_help_command("bogus").has_value());

  g_lang = Lang::en;
  REQUIRE(msg_help_command("filter").has_value());
  CHECK(!msg_help_command("bogus").has_value());

  g_lang = Lang::zh;  // 还原
}

TEST_CASE("err_help_unknown_command includes the command name and follows language") {
  g_lang = Lang::zh;
  CHECK(err_help_unknown_command("bogus").find("bogus") != std::string::npos);

  g_lang = Lang::en;
  CHECK(err_help_unknown_command("bogus").find("bogus") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// 点 2：`e` 二级子菜单只提供"当前照片"/"当前筛选"两个选项，不带任何
// 标签名——跟被退休的 g+e 流程不同，没有单一 target 概念。
TEST_CASE("msg_export_submenu_prompt offers exactly e (current) and f (filtered), no tag name") {
  g_lang = Lang::zh;
  auto prompt = msg_export_submenu_prompt();
  CHECK(prompt.find("e") != std::string::npos);
  CHECK(prompt.find("f") != std::string::npos);

  g_lang = Lang::en;
  CHECK(msg_export_submenu_prompt().find("Export current") != std::string::npos);
  CHECK(msg_export_submenu_prompt().find("Export filtered") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

TEST_CASE("filter_menu_export_no_images/success no longer take a tag name") {
  g_lang = Lang::zh;
  CHECK(!filter_menu_export_no_images().empty());
  auto success = filter_menu_export_success(3, "/tmp/out", true, 1);
  CHECK(success.find("3") != std::string::npos);
  CHECK(success.find("/tmp/out") != std::string::npos);
  CHECK(success.find("1") != std::string::npos);  // skipped count

  g_lang = Lang::en;
  CHECK(!filter_menu_export_no_images().empty());
  CHECK(filter_menu_export_success(3, "/tmp/out", false, 0).find("3") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// 点 7：`/` 前缀仍然强制要求，但错误提示要指路到 /help。
TEST_CASE("msg_console_requires_slash points users at /help") {
  g_lang = Lang::zh;
  CHECK(msg_console_requires_slash().find("/help") != std::string::npos);

  g_lang = Lang::en;
  CHECK(msg_console_requires_slash().find("/help") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// F-05:main() 的异常边界兜底提示——只验证文案本身正确拼接、跟着语言切
// 换,异常真正被捕获、终端状态被正确还原这件事只能靠真机验证(main()
// 本身不是单元测试能覆盖的粒度)。
TEST_CASE("err_internal_error includes the exception message and follows language") {
  g_lang = Lang::zh;
  CHECK(err_internal_error("disk full").find("disk full") != std::string::npos);
  CHECK(err_internal_error("disk full").find("内部错误") != std::string::npos);

  g_lang = Lang::en;
  CHECK(err_internal_error("disk full").find("disk full") != std::string::npos);
  CHECK(err_internal_error("disk full").find("internal error") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// F-03：评估失败提示——只验证文案包含图片 id 和一句能区分错误类型的原
// 因，具体措辞不是接口契约。
TEST_CASE("msg_ai_evaluation_failed includes the image id and a reason, follows language") {
  g_lang = Lang::zh;
  auto zh_text = msg_ai_evaluation_failed(42, pzt::core::EvaluationError::NetworkError);
  CHECK(zh_text.find("42") != std::string::npos);
  CHECK(zh_text.find("网络") != std::string::npos);

  auto zh_missing_key = msg_ai_evaluation_failed(1, pzt::core::EvaluationError::MissingApiKey);
  auto zh_unavailable = msg_ai_evaluation_failed(1, pzt::core::EvaluationError::ImageUnavailable);
  CHECK(zh_missing_key != zh_unavailable);  // 不同错误类型给出不同的原因文案

  g_lang = Lang::en;
  auto en_text = msg_ai_evaluation_failed(42, pzt::core::EvaluationError::NetworkError);
  CHECK(en_text.find("42") != std::string::npos);
  CHECK(en_text.find("network") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}
