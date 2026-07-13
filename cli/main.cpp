#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cli/commands/commands.h"
#include "cli/i18n/i18n.h"

// `pzt` 入口:只做子命令名分发,具体逻辑在 cli/commands(小命令 +
// browse 浏览主循环)、cli/menu(交互菜单)、cli/ui、cli/text 等模块里。
int main(int argc, char** argv) {
  using namespace pzt::cli::commands;

  pzt::cli::i18n::init_lang();

  if (argc < 2) {
    print_usage();
    return 1;
  }

  std::string subcommand = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);

  // core 用 Result<T,E> 表达预期的业务错误,异常只留给"不该发生"的场景
  // (数据库 busy、磁盘满、库损坏、扫描目录时的文件系统异常等)——但
  // "不该发生"不等于"不会发生"。这里兜底捕获,保证任何逃逸的异常都能
  // 触发正常的栈回退,让 cmd_open 内层 AltScreen/CbreakMode 这些 RAII
  // 对象的析构函数真的执行,不会把用户终端留在无回显/备用屏的坏状态。
  try {
    if (subcommand == "new") return cmd_new(args);
    if (subcommand == "list") return cmd_list(args);
    if (subcommand == "open") return cmd_open(args);
    if (subcommand == "archive") return cmd_archive(args);
    if (subcommand == "delete") return cmd_delete(args);
    if (subcommand == "rescan") return cmd_rescan(args);
    if (subcommand == "export") return cmd_export(args);
    if (subcommand == "tag") return cmd_tag(args);
    if (subcommand == "recipe") return cmd_recipe(args);
    if (subcommand == "images") return cmd_images(args);
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
