#include <cstdio>
#include <string>
#include <vector>

#include "cli/commands/commands.h"

// `pzt` 入口:只做子命令名分发,具体逻辑在 cli/commands(小命令 +
// browse 浏览主循环)、cli/menu(交互菜单)、cli/ui、cli/text 等模块里。
int main(int argc, char** argv) {
  using namespace pzt::cli::commands;

  if (argc < 2) {
    print_usage();
    return 1;
  }

  std::string subcommand = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);

  if (subcommand == "new") return cmd_new(args);
  if (subcommand == "list") return cmd_list(args);
  if (subcommand == "open") return cmd_open(args);
  if (subcommand == "archive") return cmd_archive(args);
  if (subcommand == "delete") return cmd_delete(args);
  if (subcommand == "rescan") return cmd_rescan(args);
  if (subcommand == "export") return cmd_export(args);
  if (subcommand == "tag") return cmd_tag(args);
  if (subcommand == "recipe") return cmd_recipe(args);

  std::fprintf(stderr, "pzt: 未知子命令 '%s'\n", subcommand.c_str());
  print_usage();
  return 1;
}
