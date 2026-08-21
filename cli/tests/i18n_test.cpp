#include <doctest.h>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

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

// 走 banner 的文案必须塞得进 content_cols = 终端宽 * ui_width_ratio(默认
// 0.7) - 2,80 列终端上只有 54 列,而 pad_to 超宽是静默 truncate。终端提示
// 那条原本也有个 banner 短版,真机验收之后改成进备用屏幕之前打(见
// warn_terminal_detail 的注释),现在还走 banner 的就是 B.1 这两条。
// 80 列终端 -> content_cols 54。留两列余量,这个预算的意义是"以后谁想给这
// 些走 banner 的文案加内容,先过这一关"。T-24 又加了两个使用点,提到文件作
// 用域共用一份。
const std::size_t kNarrowestBanner = 52;

TEST_CASE("messages that go through the banner fit the narrowest common terminal") {
  auto banner_width = [](std::string s) {
    // 这两条自带结尾换行(原本是 fprintf 用的),browse.cpp 进 banner 前会剥
    // 掉,这里按剥掉之后的宽度量。
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return pzt::cli::text::display_width(s);
  };

  for (Lang lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    CHECK(banner_width(err_open_render_failed()) <= kNarrowestBanner);
    CHECK(banner_width(err_open_decode_failed()) <= kNarrowestBanner);
  }

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
  // 2026-07-30 真机验收:Terminal.app 下图片区是满屏乱码,不是空白(它把
  // APC 序列当普通文本打出来了)。原文案只说"空白",在真机上是错的。
  CHECK(zh.find("乱码") != std::string::npos);
  // 详情版打在真实终端上,换行由调用方补,文案自己不带。
  CHECK(zh.back() != '\n');

  g_lang = Lang::en;
  std::string en = warn_terminal_detail();
  CHECK(en.find("Ghostty") != std::string::npos);
  CHECK(en.find("may not") != std::string::npos);
  CHECK(en.find("warn_unsupported_terminal") != std::string::npos);
  CHECK(en.find("garbage") != std::string::npos);
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

// T-15（#30）：`.`（当前视图）这一支的作用域错误有自己的文案，不能复用
// err_console_invalid_scope。整条决策（PRD #28 D-6 第二条）就是"`.` 是合
// 法写法，报语法错是撒谎"，两句话说成一句等于把那条决策原地取消，所以这
// 里连内容一起断言，不只断言两种语言都非空。
TEST_CASE("err_console_scope_no_view is its own text, not the invalid-scope one") {
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    CHECK(err_console_scope_no_view() != err_console_invalid_scope());
    // 不该重复"必须是 * 或 #标签名"那套说法 - 用户写的 `.` 本来就没错。
    CHECK(err_console_scope_no_view().find("#") == std::string::npos);
  }

  g_lang = Lang::zh;
  CHECK(err_console_scope_no_view().find("视图") != std::string::npos);

  g_lang = Lang::en;
  CHECK(err_console_scope_no_view().find("view") != std::string::npos);

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

// T-15 票 D（#32 验收）：`.` 接进 `/dedup` 与 `/ai_eval` 之后，两条命令
// 的 `/help` 详情必须说出这种写法 - `.` 是**没有别处可发现**的：`*` 与
// `#标签` 用户在别处见得到(标签栏、filter 提示)，一个孤零零的点只可能从
// 文档或这里学到。文案漏了它，等于这一票只做了一半。
TEST_CASE("msg_help_command documents the `.` (current view) scope for dedup and ai_eval") {
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    auto dedup = msg_help_command("dedup");
    REQUIRE(dedup.has_value());
    CHECK(dedup->find("/dedup .") != std::string::npos);

    auto ai_eval = msg_help_command("ai_eval");
    REQUIRE(ai_eval.has_value());
    CHECK(ai_eval->find("/ai_eval .") != std::string::npos);
  }

  g_lang = Lang::zh;  // 还原
}

// 同上的另一半：`.` 成为合法写法之后，"范围写错了"那句话不能还在只列
// `*` 和 `#标签名` - 用户照它改，改出来的还是不含 `.` 的两种写法。这跟
// err_console_scope_no_view 那条是同一个"不撒谎"的要求(PRD #28 D-6)。
TEST_CASE("err_console_invalid_scope lists `.` alongside the other two forms") {
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    CHECK(err_console_invalid_scope().find("*") != std::string::npos);
    CHECK(err_console_invalid_scope().find(".") != std::string::npos);
    CHECK(err_console_invalid_scope().find("#") != std::string::npos);
  }

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

// T-18：`:` 控制台是 /dedup(近似重复检测的唯一交互入口)、/ai_eval、/filter、
// /tasks 的唯一入口，此前在 usage 里一个字都没有 - 只看 usage 的人不会知道
// 去重功能存在。这条钉住"入口本身可发现"，不钉具体命令清单的排版。
TEST_CASE("usage_main advertises the : console and the commands behind it") {
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    auto usage = usage_main();
    // 不能只找 ":" - usage 头一行就是 "usage:"，那样断言恒真，钉不住任何东西。
    // 要找的是把 `:` 讲成一个按键的那处文字。
    bool advertises_console =
        usage.find(": 控制台") != std::string::npos || usage.find(": Console") != std::string::npos;
    CHECK(advertises_console);
    CHECK(usage.find("/dedup") != std::string::npos);
    CHECK(usage.find("/ai_eval") != std::string::npos);
    CHECK(usage.find("/filter") != std::string::npos);
    CHECK(usage.find("/tasks") != std::string::npos);
    CHECK(usage.find("/help") != std::string::npos);
  }
  g_lang = Lang::zh;  // 还原
}

// T-18：这句空标签提示曾把用户指向 `pzt tag create`，而 cmd_tag 只 dispatch
// list/apply/clear - 那个命令从来不存在。标签只能在 `pzt open` 里建(space 进
// 标签菜单、c 新建)，提示必须指向真实存在的路径。
TEST_CASE("msg_tag_list_empty points at a path that actually exists") {
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    auto msg = msg_tag_list_empty();
    CHECK(msg.find("tag create") == std::string::npos);
    CHECK(msg.find("pzt open") != std::string::npos);
    CHECK(msg.find("space") != std::string::npos);
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

// T-23：文案原来只有裸数据库 ID("图 47")，而用户手上只有文件名，界面
// 其它每一处也都用 file_name 展示,那个数字无从对照。现在优先报文件名，
// 只在图片记录查不到(比如"图片已不存在"这类失败本身)时才回落到 ID。
TEST_CASE("msg_ai_evaluation_failed reports the file name and a reason, follows language") {
  g_lang = Lang::zh;
  auto zh_text =
      msg_ai_evaluation_failed("IMG_0042.JPG", 42, pzt::core::EvaluationError::NetworkError, 1);
  CHECK(zh_text.find("IMG_0042.JPG") != std::string::npos);
  CHECK(zh_text.find("网络") != std::string::npos);

  auto zh_missing_key =
      msg_ai_evaluation_failed("a.jpg", 1, pzt::core::EvaluationError::MissingApiKey, 1);
  auto zh_unavailable =
      msg_ai_evaluation_failed("a.jpg", 1, pzt::core::EvaluationError::ImageUnavailable, 1);
  CHECK(zh_missing_key != zh_unavailable);  // 不同错误类型给出不同的原因文案

  g_lang = Lang::en;
  auto en_text =
      msg_ai_evaluation_failed("IMG_0042.JPG", 42, pzt::core::EvaluationError::NetworkError, 1);
  CHECK(en_text.find("IMG_0042.JPG") != std::string::npos);
  CHECK(en_text.find("network") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// 文件名查不到时不能把这条提示整个吞掉 - 回落到 ID 至少还能对照
// `pzt images` 的输出，比什么都不说强。
TEST_CASE("msg_ai_evaluation_failed falls back to the image id when the file name is unknown") {
  g_lang = Lang::zh;
  auto zh_text =
      msg_ai_evaluation_failed(std::nullopt, 47, pzt::core::EvaluationError::ImageUnavailable, 1);
  CHECK(zh_text.find("47") != std::string::npos);

  g_lang = Lang::en;
  auto en_text =
      msg_ai_evaluation_failed(std::nullopt, 47, pzt::core::EvaluationError::ImageUnavailable, 1);
  CHECK(en_text.find("47") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// T-23 的另一半：一次批量评估里失败不止一条时，光报最近那一条会让用户
// 以为只错了一张。累计失败张数要出现在文案里，那才是"值得停下来检查环
// 境"的信号；最近一条仍然报出来，因为它带着具体原因。
TEST_CASE("msg_ai_evaluation_failed reports the running total when more than one has failed") {
  g_lang = Lang::zh;
  auto zh_many =
      msg_ai_evaluation_failed("IMG_0042.JPG", 42, pzt::core::EvaluationError::NetworkError, 37);
  CHECK(zh_many.find("37") != std::string::npos);
  CHECK(zh_many.find("IMG_0042.JPG") != std::string::npos);
  CHECK(zh_many.find("网络") != std::string::npos);
  auto zh_one =
      msg_ai_evaluation_failed("IMG_0042.JPG", 42, pzt::core::EvaluationError::NetworkError, 1);
  CHECK(zh_many != zh_one);
  CHECK(zh_one.find("37") == std::string::npos);

  g_lang = Lang::en;
  auto en_many =
      msg_ai_evaluation_failed("IMG_0042.JPG", 42, pzt::core::EvaluationError::NetworkError, 37);
  CHECK(en_many.find("37") != std::string::npos);
  CHECK(en_many.find("IMG_0042.JPG") != std::string::npos);

  g_lang = Lang::zh;  // 还原
}

// `/tasks` 原来只报排队/处理中，失败过多少张一处都查不到 - 状态行是一
// 次性的，错过就没了。累计失败数挂在这里，用户任何时候都能回看。
TEST_CASE("msg_ai_tasks_status mentions the cumulative failure count only when there is one") {
  g_lang = Lang::zh;
  auto zh_clean = msg_ai_tasks_status(3, true, 0);
  CHECK(zh_clean.find("3") != std::string::npos);
  auto zh_failed = msg_ai_tasks_status(3, true, 12);
  CHECK(zh_failed.find("12") != std::string::npos);
  CHECK(zh_failed.find("失败") != std::string::npos);

  g_lang = Lang::en;
  auto en_clean = msg_ai_tasks_status(3, true, 0);
  auto en_failed = msg_ai_tasks_status(3, true, 12);
  CHECK(en_failed.find("12") != std::string::npos);
  CHECK(en_failed.find("failed") != std::string::npos);
  CHECK(en_clean != en_failed);

  g_lang = Lang::zh;  // 还原
}

// T-24：超出上限的标签在菜单里选不到,以前是完全静默的。截断本身保留(老项
// 目可能已经有 8 个以上),但必须报出被藏起来的数量。
TEST_CASE("tag/filter 菜单在有标签被截断时报出隐藏数量,没截断时不加噪音") {
  for (Lang lang : {Lang::zh, Lang::en}) {
    g_lang = lang;

    CHECK(tag_menu_actions_line(/*at_limit=*/false, /*hidden=*/0).find("(+") == std::string::npos);
    CHECK(tag_menu_actions_line(/*at_limit=*/true, /*hidden=*/2).find("+2") != std::string::npos);

    CHECK(filter_menu_actions_line(/*hidden=*/0).find("(+") == std::string::npos);
    CHECK(filter_menu_actions_line(/*hidden=*/2).find("+2") != std::string::npos);
  }

  g_lang = Lang::zh;  // 还原
}

// T-24：这两条注记必须挂在操作行,不能挂在编号行。编号行排满 8 个标签就要
// 100 列开外,而 content_cols 只有终端宽度的 70%(120 列的终端上是 82),
// pad_to 从尾部截,挂在编号行的话恰恰在标签最多、最该提示的时候第一个被切
// 掉。这条用例守的就是这件事:操作行连同注记必须能在窄终端里放下。
TEST_CASE("菜单操作行连同 T-24 的注记一起,在窄终端里放得下") {
  for (Lang lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    // 最坏情况:已满标记和隐藏数量同时出现,数量取两位数。
    CHECK(pzt::cli::text::display_width(tag_menu_actions_line(true, 12)) <= kNarrowestBanner);
    CHECK(pzt::cli::text::display_width(filter_menu_actions_line(12)) <= kNarrowestBanner);
  }

  g_lang = Lang::zh;  // 还原
}

// T-24：`c` 新建在已满时会被挡住,菜单上就得先说清楚,不能让用户按下去才
// 知道-跟 recipe_menu 那条 version 上限一样的处理。
TEST_CASE("tag_menu_actions_line 在已满时标记 c,未满时不标记") {
  for (Lang lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    auto normal = tag_menu_actions_line(/*at_limit=*/false, /*hidden=*/0);
    auto full = tag_menu_actions_line(/*at_limit=*/true, /*hidden=*/0);
    CHECK(normal != full);
    // 两种状态下 c/d/Esc 三个键位都还在,已满只是加标记,不是把选项拿掉
    // -键位消失会让"按 c 得到一句解释"这条路径自己先没了。
    for (const auto& line : {normal, full}) {
      CHECK(line.find("c") != std::string::npos);
      CHECK(line.find("d") != std::string::npos);
      CHECK(line.find("Esc") != std::string::npos);
    }
  }

  g_lang = Lang::zh;  // 还原
}

// T-24：挡住之后必须说清楚上限是多少、怎么才能继续建,否则只是把静默截断
// 换成一句静默拒绝。
TEST_CASE("tag_menu_limit_reached 报出上限数字并指出出路") {
  g_lang = Lang::zh;
  auto zh = tag_menu_limit_reached(8);
  CHECK(zh.find("8") != std::string::npos);
  CHECK(zh.find("d") != std::string::npos);  // 指向 space d 删除标签定义
  CHECK(pzt::cli::text::display_width(zh) <= kNarrowestBanner);

  g_lang = Lang::en;
  auto en = tag_menu_limit_reached(8);
  CHECK(en.find("8") != std::string::npos);
  CHECK(en.find("d") != std::string::npos);
  CHECK(pzt::cli::text::display_width(en) <= kNarrowestBanner);

  g_lang = Lang::zh;  // 还原
}

// 从一份 C++ 源码里抠出所有字符串字面量的内容。左到右单趟扫描,维护"当前
// 是否在字符串里"这一个状态:不在字符串里遇到 `//` 就丢掉本行剩下的部分
// (注释里带引号的地方非常多-i18n.cpp 的注释经常整段引用文案,不先剥掉注释
// 的话它们会被当成字面量,守卫立刻变成误报机)。`\"` 按转义处理,不结束字
// 符串。字符字面量('r' 这种)不含要查的词,不特殊处理。
std::vector<std::string> string_literals_in(const std::string& path) {
  std::ifstream in(path);
  REQUIRE_MESSAGE(in.is_open(), "打不开 " << path << " - 守卫会静默失效,不能当噪音跳过");

  std::vector<std::string> literals;
  std::string line;
  while (std::getline(in, line)) {
    bool in_string = false;
    std::string current;
    for (std::size_t i = 0; i < line.size(); ++i) {
      if (in_string && line[i] == '\\' && i + 1 < line.size()) {
        current += line[i];
        current += line[i + 1];
        ++i;  // 转义序列整体跳过,`\"` 不算收尾引号
        continue;
      }
      if (line[i] == '"') {
        if (in_string) literals.push_back(current);
        current.clear();
        in_string = !in_string;
        continue;
      }
      if (!in_string && line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
        break;  // 行注释,本行到此为止
      }
      if (in_string) current += line[i];
    }
  }
  return literals;
}

// T-15 票 E：cli 显示文案里表示 recipe 这个概念的词统一成"配方"/Recipe。
// 依据是 ADR-0002：配方(recipe)是 cli/core 的词-精确、可执行的那个调色对
// 象;风格(style)是 agent 的词-用户口语里"我想要什么感觉",模糊、不可直接
// 执行。坐在终端前按 r 的人做的是"在 10 个预设里选第 3 个",他选的是精确
// 对象,不是在表达偏好,所以 cli 这一侧不该出现"风格"。
//
// 守卫扫的是 i18n.cpp 里的字符串字面量,不是挨个调用几个函数-枚举只能盖住
// 立项时就存在的那几条,拦不住以后新写的文案,而票 C 马上要写一批新文案。
// 注释不在扫描范围内:那里面的"风格"多数是"vim 风格"/"留白风格"这种通用词
// 义,本来就不该改。
TEST_CASE("T-15：cli 显示文案里不出现\"风格\"/Style") {
  auto literals = string_literals_in(PZT_I18N_SOURCE);
  // 扫出来的量级不对就说明扫描器本身坏了(比如路径变了、或者剥注释剥过头),
  // 那样后面每条断言都会"通过",守卫静默失效。
  REQUIRE(literals.size() > 100);

  for (const auto& s : literals) {
    CHECK(s.find("风格") == std::string::npos);
    CHECK(s.find("Style") == std::string::npos);
  }
}

// 上面那条只保证"风格"没了。光删词不算统一-概念本身还得在文案里露出来,
// 否则用户在菜单和 usage 里都找不到这个功能。
TEST_CASE("T-15：配方这个概念仍然出现在菜单、usage 和报错里") {
  g_lang = Lang::zh;
  CHECK(info_style_label() == "配方:");
  CHECK(usage_main().find("配方") != std::string::npos);
  CHECK(export_skip_reason(pzt::core::SkipReason::RenderFailed).find("配方") !=
        std::string::npos);
  bool zh_menu_mentions_recipe = false;
  for (const auto& line : menu_lines()) {
    if (line.text.find("配方") != std::string::npos) zh_menu_mentions_recipe = true;
  }
  CHECK(zh_menu_mentions_recipe);

  g_lang = Lang::en;
  CHECK(info_style_label() == "Recipe:");
  bool en_menu_mentions_recipe = false;
  for (const auto& line : menu_lines()) {
    if (line.text.find("Recipe") != std::string::npos) en_menu_mentions_recipe = true;
  }
  CHECK(en_menu_mentions_recipe);

  g_lang = Lang::zh;  // 还原
}

// T-15 票 C 决策 D-14：批量菜单是 `r` 菜单的**子集**。`v`/`c`/`d` 不是
// "画上去按下给一句不可用",是根本不出现在图例上——三个选项各自的理由见
// issue #33。图例是用户判断"这个菜单能做什么"的唯一依据,多画一个就是在
// 承诺一件做不到的事。
TEST_CASE("T-15：批量配方菜单的图例只有清除和取消,没有 v/c/d") {
  // 图例里每个选项都是 menu_item 拼的 "键:[文案]",且前面必有空格(行首一
  // 个、选项之间两个)，所以连空格一起找。只找 "键:[" 不带空格会误报:
  // "Esc:[" 里就含着一个 "c:["，那会让这条守卫在一份完全正确的图例上红。
  // 只找单个字母就更糟,任何文案里的 c 都会命中。
  auto has_key = [](const std::string& line, const std::string& key) {
    return line.find(" " + key + ":[") != std::string::npos;
  };
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    auto batch = recipe_menu_actions_line_batch();
    CHECK_FALSE(has_key(batch, "v"));
    CHECK_FALSE(has_key(batch, "c"));
    CHECK_FALSE(has_key(batch, "d"));
    // 清除留着:D-8 定了不做撤销,批量清除是唯一的兜底路径。
    CHECK(has_key(batch, "r"));
    CHECK(has_key(batch, "Esc"));
  }
  g_lang = Lang::zh;
}

// 单张那份必须原样保留 v/c/d,否则"子集"这个说法就变成了"两边都砍了"。
TEST_CASE("T-15：单张配方菜单的图例仍然带 v/c/d") {
  g_lang = Lang::zh;
  auto single = recipe_menu_actions_line(/*has_recipe=*/true);
  CHECK(single.find(" v:[") != std::string::npos);
  CHECK(single.find(" c:[") != std::string::npos);
  CHECK(single.find(" d:[") != std::string::npos);
}

// `/recipe` 得能在 /help 里被找到。总览漏一条的失效模式是"功能上线了但没
// 人知道",而这正是控制台命令唯一的发现入口。
TEST_CASE("T-15：/help 总览与详情都认识 /recipe") {
  for (auto lang : {Lang::zh, Lang::en}) {
    g_lang = lang;
    CHECK(msg_help_overview().find("/recipe") != std::string::npos);
    auto detail = msg_help_command("recipe");
    REQUIRE(detail.has_value());
    // 详情要说清两件事:作用域写在命令里、配方在回车之后选(决策 D-2)。
    CHECK(detail->find("/recipe *") != std::string::npos);
    CHECK(detail->find("/recipe .") != std::string::npos);
  }
  g_lang = Lang::zh;
}

// 确认文案的主角是 M(见 D-9)。N 与 M 都必须真的出现在第一行里——把数字
// 漏掉的失效模式是用户看着一句没有量的话按 y。
TEST_CASE("T-15：批量确认第一行同时报出 N 与 M") {
  g_lang = Lang::zh;
  auto apply_line = msg_recipe_batch_confirm_line1(90, 7, std::string("City Pop"));
  CHECK(apply_line.find("90") != std::string::npos);
  CHECK(apply_line.find("7") != std::string::npos);
  CHECK(apply_line.find("City Pop") != std::string::npos);

  // M=0 时文案照出(D-9 否决了"只在 M>0 时确认"),只是不可逆那部分为零。
  CHECK(msg_recipe_batch_confirm_line1(90, 0, std::string("City Pop")).find("90") !=
        std::string::npos);

  // 批量清除走同一条路,没有配方名。
  auto clear_line = msg_recipe_batch_confirm_line1(90, 7, std::nullopt);
  CHECK(clear_line.find("90") != std::string::npos);
  CHECK(clear_line.find("7") != std::string::npos);
}
