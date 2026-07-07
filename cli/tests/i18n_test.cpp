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
  CHECK(banner_text().find("上一张") != std::string::npos);
  CHECK(info_tags_label() == "标签:");

  g_lang = Lang::en;
  CHECK(banner_text().find("Prev") != std::string::npos);
  CHECK(info_tags_label() == "Tags:");

  g_lang = Lang::zh;  // 还原成默认值,不泄漏状态给其它测试用例
}
