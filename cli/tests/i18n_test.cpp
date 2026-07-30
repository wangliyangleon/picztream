#include <doctest.h>
#include <cstdlib>
#include <string>

#include "cli/i18n/i18n.h"
#include "cli/text/text.h"

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

// T-10 (a)：终端可能不支持 Kitty 图像协议时的提示,分 banner 版与详情版。
// 分两条不是啰嗦:banner 那一行的可用宽度是 content_cols = 终端宽 *
// ui_width_ratio(默认 0.7) - 2,80 列终端上只有 54 列,而 pad_to 会静默
// truncate。完整文案(含关掉提示的办法)有 160+ 列,塞进 banner 只会被截成
// 半句,恰好把最该看到的那半句截掉。
TEST_CASE("terminal warning banner line fits the narrowest common terminal") {
  // 80 列终端 -> content_cols 54。留两列余量,并且这条断言的意义就是"以后
  // 谁想给这句话加内容,先过这一关"。
  const std::size_t kNarrowestBanner = 52;

  g_lang = Lang::zh;
  std::string zh = warn_terminal_banner();
  CHECK(pzt::cli::text::display_width(zh) <= kNarrowestBanner);
  // PRD 决策 4：判定是白名单猜的,措辞必须是"可能",不能断言用户的终端不行。
  CHECK(zh.find("可能") != std::string::npos);
  // banner 是单行,自带换行会把边框顶乱。
  CHECK(zh.find('\n') == std::string::npos);

  g_lang = Lang::en;
  std::string en = warn_terminal_banner();
  CHECK(pzt::cli::text::display_width(en) <= kNarrowestBanner);
  CHECK(en.find("may not") != std::string::npos);
  CHECK(en.find('\n') == std::string::npos);

  g_lang = Lang::zh;  // 还原
}

TEST_CASE("terminal warning detail names the fix and points at the off switch") {
  g_lang = Lang::zh;
  std::string zh = warn_terminal_detail();
  // 必须指名修复动作,否则这句话只是"出事了"而不解决任何问题。
  CHECK(zh.find("Ghostty") != std::string::npos);
  CHECK(zh.find("可能") != std::string::npos);
  // 白名单必然有假阴性,逃生口要写在提示里,否则对这些用户就是永久噪音。
  CHECK(zh.find("warn_unsupported_terminal") != std::string::npos);
  // 详情版打在真实终端上,换行由调用方补,文案自己不带。
  CHECK(zh.back() != '\n');

  g_lang = Lang::en;
  std::string en = warn_terminal_detail();
  CHECK(en.find("Ghostty") != std::string::npos);
  CHECK(en.find("may not") != std::string::npos);
  CHECK(en.find("warn_unsupported_terminal") != std::string::npos);
  CHECK(en.back() != '\n');

  g_lang = Lang::zh;  // 还原
}

// F-11：dedup 结果文案在实际标记到重复图片时带上"按 f 9 查看"入口提
// 示；标记数为 0 时不带（范围内没有新重复组，提示了也是空列表）。
// 键名断言的是 f 不是 g:筛选入口从 g 改成 f 时这句文案漏改了，真机反馈
// (2026-07-29)才发现，而这条用例当时还在断言旧键、跟着一起绿——所以现在
// 连"不许再出现 g 9"一起断言,别再被同一个方式绕过去。
TEST_CASE("msg_dedup_result includes entry hint only when images were tagged") {
  g_lang = Lang::zh;
  auto zh_tagged = msg_dedup_result(2, 4, 0, 0);
  CHECK(zh_tagged.find("2") != std::string::npos);
  CHECK(zh_tagged.find("4") != std::string::npos);
  CHECK(zh_tagged.find("f 9") != std::string::npos);
  CHECK(zh_tagged.find("g 9") == std::string::npos);

  auto zh_empty = msg_dedup_result(0, 0, 0, 0);
  CHECK(zh_empty.find("f 9") == std::string::npos);

  g_lang = Lang::en;
  auto en_tagged = msg_dedup_result(2, 4, 0, 0);
  CHECK(en_tagged.find("f 9") != std::string::npos);
  CHECK(en_tagged.find("g 9") == std::string::npos);

  auto en_empty = msg_dedup_result(0, 0, 0, 0);
  CHECK(en_empty.find("f 9") == std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// F-08：范围内有图片因为没有拍摄时间被跳过时,结果文案带一句提示;
// 没有跳过的常见路径不受影响,不多带一句空话。
TEST_CASE("msg_dedup_result mentions skipped-no-capture-time count only when nonzero") {
  g_lang = Lang::zh;
  auto zh_skipped = msg_dedup_result(1, 2, 3, 0);
  CHECK(zh_skipped.find("3") != std::string::npos);
  CHECK(zh_skipped.find("拍摄时间") != std::string::npos);

  auto zh_none = msg_dedup_result(1, 2, 0, 0);
  CHECK(zh_none.find("拍摄时间") == std::string::npos);

  g_lang = Lang::en;
  auto en_skipped = msg_dedup_result(1, 2, 3, 0);
  CHECK(en_skipped.find("capture time") != std::string::npos);

  auto en_none = msg_dedup_result(1, 2, 0, 0);
  CHECK(en_none.find("capture time") == std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// W2026-07-21 目标二 + `/dedup --ai`：某几组因为 AI 比较失败而退化成"保
// 留最新的一张"时带一句说明,不开 --ai 的常见路径(恒为 0)不受影响。
TEST_CASE("msg_dedup_result mentions ai fallback count only when nonzero") {
  g_lang = Lang::zh;
  auto zh_fallback = msg_dedup_result(4, 8, 0, 2);
  CHECK(zh_fallback.find("2 组 AI 比较失败") != std::string::npos);

  auto zh_none = msg_dedup_result(4, 8, 0, 0);
  CHECK(zh_none.find("AI") == std::string::npos);

  g_lang = Lang::en;
  auto en_fallback = msg_dedup_result(4, 8, 0, 2);
  CHECK(en_fallback.find("AI comparison failed") != std::string::npos);

  auto en_none = msg_dedup_result(4, 8, 0, 0);
  CHECK(en_none.find("AI") == std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// 跳过计数和 AI 退化计数可能同时出现在同一行里,两段提示都不能借用对方
// 的关键词,否则用户分不清哪个数字说的是哪件事。
TEST_CASE("msg_dedup_result keeps the skipped and ai-fallback notes distinguishable") {
  g_lang = Lang::zh;
  auto zh_both = msg_dedup_result(4, 8, 3, 2);
  CHECK(zh_both.find("3 张因无拍摄时间") != std::string::npos);
  CHECK(zh_both.find("2 组 AI 比较失败") != std::string::npos);
  // AI 退化那句不许再提"拍摄时间",不然一行里出现两次、指代不清。
  CHECK(zh_both.find("拍摄时间") == zh_both.rfind("拍摄时间"));

  g_lang = Lang::en;
  auto en_both = msg_dedup_result(4, 8, 3, 2);
  CHECK(en_both.find("no capture time") != std::string::npos);
  CHECK(en_both.find("AI comparison failed") != std::string::npos);
  CHECK(en_both.find("capture time") == en_both.rfind("capture time"));

  g_lang = Lang::zh;  // 还原
}

// 反馈:退出时如果评估队列里还有任务,提示文案要带上数量,按键提示要
// 明确"y 才是真的退出"。
TEST_CASE("msg_quit_confirm_pending includes the pending count and follows language") {
  g_lang = Lang::zh;
  CHECK(msg_quit_confirm_pending_line1(3).find("3") != std::string::npos);
  CHECK(msg_quit_confirm_pending_line2().find("y") != std::string::npos);

  g_lang = Lang::en;
  CHECK(msg_quit_confirm_pending_line1(3).find("3") != std::string::npos);
  CHECK(msg_quit_confirm_pending_line2().find("y") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// 反馈:标签+前缀太长会被截断,改成不带标签的 "TagName | criterion" 紧
// 凑写法——控制台二级筛选四个关键字各自映射到对应的中文词,英文路径原
// 样透出关键字(不额外维护一份英文词表);两层筛选各自可以单独出现,也
// 可以同时出现;都不生效时返回空串。
TEST_CASE("info_active_filters_label joins tag name and console criterion compactly") {
  g_lang = Lang::zh;
  CHECK(info_active_filters_label(std::nullopt, std::nullopt).empty());

  auto tag_only = info_active_filters_label(std::string("Food"), std::nullopt);
  CHECK(tag_only.find("Food") != std::string::npos);
  CHECK(tag_only.find("|") == std::string::npos);  // 只有一段时不出现分隔符

  CHECK(info_active_filters_label(std::nullopt, std::string("unevaluated")).find("未评估") !=
        std::string::npos);
  CHECK(info_active_filters_label(std::nullopt, std::string("fail")).find("不达标") !=
        std::string::npos);
  CHECK(info_active_filters_label(std::nullopt, std::string("reject")).find("废片") !=
        std::string::npos);
  CHECK(info_active_filters_label(std::nullopt, std::string("dup")).find("重复") != std::string::npos);

  auto both = info_active_filters_label(std::string("Food"), std::string("fail"));
  CHECK(both.find("Food") != std::string::npos);
  CHECK(both.find("不达标") != std::string::npos);
  CHECK(both.find(" | ") != std::string::npos);  // 两段同时出现时用竖杠隔开

  g_lang = Lang::en;
  CHECK(info_active_filters_label(std::nullopt, std::string("unevaluated"))
            .find("unevaluated") != std::string::npos);

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

// 点 2：`e` 二级子菜单不带任何标签名——跟被退休的 g+e 流程不同，没有
// 单一 target 概念。`a`(全部)始终出现；`f`(筛选结果)只在 filter_active
// 时才该出现在提示里，没有筛选时选它没有意义。
TEST_CASE("msg_export_submenu_prompt offers e/a always, f only when filter_active") {
  g_lang = Lang::zh;
  auto prompt_active = msg_export_submenu_prompt(true);
  CHECK(prompt_active.find("e") != std::string::npos);
  CHECK(prompt_active.find("a") != std::string::npos);
  CHECK(prompt_active.find("f") != std::string::npos);

  auto prompt_inactive = msg_export_submenu_prompt(false);
  CHECK(prompt_inactive.find("e") != std::string::npos);
  CHECK(prompt_inactive.find("a") != std::string::npos);
  CHECK(prompt_inactive.find("f") == std::string::npos);

  g_lang = Lang::en;
  CHECK(msg_export_submenu_prompt(true).find("Export current") != std::string::npos);
  CHECK(msg_export_submenu_prompt(true).find("Export all") != std::string::npos);
  CHECK(msg_export_submenu_prompt(true).find("Export filtered") != std::string::npos);
  CHECK(msg_export_submenu_prompt(false).find("Export filtered") == std::string::npos);

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

// `--all-keep`(headless)对应的"0 张"提示,跟按标签名的 msg_export_no_images
// 区别只在于没有标签名可带。
TEST_CASE("msg_export_no_images_all is non-empty in both languages") {
  g_lang = Lang::zh;
  CHECK(!msg_export_no_images_all().empty());

  g_lang = Lang::en;
  CHECK(!msg_export_no_images_all().empty());

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
TEST_CASE("msg_unknown_key names the key pressed and points at the real filter key") {
  g_lang = Lang::zh;
  auto zh_text = msg_unknown_key('g');
  CHECK(zh_text.find("g") != std::string::npos);
  CHECK(zh_text.find("f") != std::string::npos);  // 指向真正的筛选键

  g_lang = Lang::en;
  auto en_text = msg_unknown_key('g');
  CHECK(en_text.find("g") != std::string::npos);
  CHECK(en_text != zh_text);

  g_lang = Lang::zh;  // 还原
}

// usage 里的按键说明必须跟 browse.cpp 主循环实际接受的键一致。这条长期
// 不一致(写 g、实际是 f)，是 T-3 的起因。
TEST_CASE("usage_main advertises f as the filter key, not g") {
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    auto usage = usage_main();
    bool advertises_f =
        usage.find("f 筛选") != std::string::npos || usage.find("f Filter") != std::string::npos;
    CHECK(advertises_f);
    CHECK(usage.find("g 筛选") == std::string::npos);
    CHECK(usage.find("g Filter") == std::string::npos);
  }
  g_lang = Lang::zh;  // 还原
}

TEST_CASE("err_db_schema_too_new carries both versions and follows language") {
  g_lang = Lang::zh;
  auto zh_text = err_db_schema_too_new(2, 1);
  CHECK(zh_text.find("v2") != std::string::npos);
  CHECK(zh_text.find("v1") != std::string::npos);
  CHECK(zh_text.find("brew upgrade pzt") != std::string::npos);

  g_lang = Lang::en;
  auto en_text = err_db_schema_too_new(2, 1);
  CHECK(en_text.find("v2") != std::string::npos);
  CHECK(en_text.find("v1") != std::string::npos);
  CHECK(en_text != zh_text);

  g_lang = Lang::zh;  // 还原
}

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
