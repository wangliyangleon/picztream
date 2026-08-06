#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cli/commands/commands.h"
#include "cli/i18n/i18n.h"
#include "core/api.h"

// `pzt` 入口:只做子命令名分发,具体逻辑在 cli/commands(小命令 +
// browse 浏览主循环)、cli/menu(交互菜单)、cli/ui、cli/text 等模块里。
int main(int argc, char** argv) {
  using namespace pzt::cli::commands;

  pzt::cli::i18n::init_lang();

  // AI 请求的超时上限：core 不读 Settings，由 cli 读一次推进去(同
  // LocalModelConfig 那条"可调行为参数由调用方显式传入"的约定)。放在这
  // 里而不是各个 AI 命令里，是因为漏一处的后果是那条路径静默沿用默认值，
  // 而这种漏很难被测出来。语义与默认值见 core/ai/ai.h。
  pzt::core::ai::set_request_timeout_seconds(
      pzt::core::load_settings().ai_request_timeout_seconds);

  if (argc < 2) {
    print_usage();
    return 1;
  }

  std::string subcommand = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);

  // 版本查询与求助都是无副作用的即时命令,不进下面的子命令 try 块。
  if (subcommand == "--version" || subcommand == "version") {
    print_version();
    return 0;
  }
  // 三种写法都接:`--help` 是 GNU 惯例,`-h` 是短写,`help` 是子命令式。此前
  // 一种都不认,全部落到最后那句"未知子命令"上 - 报错 + 退出码 1,把求助当
  // 成用错。
  if (subcommand == "--help" || subcommand == "-h" || subcommand == "help") {
    print_help();
    return 0;
  }

  // core 用 Result<T,E> 表达预期的业务错误,异常只留给"不该发生"的场景
  // (数据库 busy、磁盘满、库损坏、扫描目录时的文件系统异常等)——但
  // "不该发生"不等于"不会发生"。这里兜底捕获,保证任何逃逸的异常都能
  // 触发正常的栈回退,让 cmd_open 内层 AltScreen/CbreakMode 这些 RAII
  // 对象的析构函数真的执行,不会把用户终端留在无回显/备用屏的坏状态。
  // 这里只覆盖异常路径;信号路径不做栈回退,由 cli/term/signal_restore.h
  // 单独接管。
  try {
    if (subcommand == "new") return cmd_new(args);
    if (subcommand == "list") return cmd_list(args);
    if (subcommand == "open") return cmd_open(args);
    if (subcommand == "delete") return cmd_delete(args);
    if (subcommand == "rescan") return cmd_rescan(args);
    if (subcommand == "export") return cmd_export(args);
    if (subcommand == "tag") return cmd_tag(args);
    if (subcommand == "recipe") return cmd_recipe(args);
    if (subcommand == "images") return cmd_images(args);
    if (subcommand == "dedup") return cmd_dedup(args);
    if (subcommand == "export-images") return cmd_export_images(args);
    if (subcommand == "curate") return cmd_curate(args);
  } catch (const pzt::core::SchemaTooNewError& e) {
    // 必须排在下面那条 catch (const std::exception&) 之前:SchemaTooNewError
    // 继承自 std::exception,handler 按源码顺序匹配,放在基类后面就是死代码。
    // 这是唯一一个我们认得出、能给出可操作提示的逃逸异常。
    std::fprintf(stderr, "%s",
                 pzt::cli::i18n::err_db_schema_too_new(e.found_version(), e.supported_version())
                     .c_str());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_internal_error(e.what()).c_str());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "%s", pzt::cli::i18n::err_internal_error("unknown").c_str());
    return 1;
  }

  std::fprintf(stderr, "%s", pzt::cli::i18n::err_unknown_subcommand(subcommand).c_str());
  print_usage();
  return 1;
}
