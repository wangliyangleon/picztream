#include <doctest.h>
#include <cstdlib>
#include <string>

#include "cli/i18n/i18n.h"

using namespace pzt::cli::i18n;

TEST_CASE("i18n language initialization and switching") {
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
