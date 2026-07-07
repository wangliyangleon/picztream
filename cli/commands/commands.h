#pragma once

#include <string>
#include <vector>

// `pzt` 各子命令的入口。每个 cmd_* 接收 argv[2:] 的位置参数,返回进程退出
// 码。main() 只做子命令名分发,具体逻辑在 commands.cpp(小命令)和
// browse.cpp(`pzt open` 浏览主循环)里。
namespace pzt::cli::commands {

void print_usage();

int cmd_new(const std::vector<std::string>& args);
int cmd_list(const std::vector<std::string>& args);
int cmd_open(const std::vector<std::string>& args);
int cmd_archive(const std::vector<std::string>& args);
int cmd_delete(const std::vector<std::string>& args);
int cmd_rescan(const std::vector<std::string>& args);
int cmd_export(const std::vector<std::string>& args);
int cmd_tag(const std::vector<std::string>& args);
int cmd_recipe(const std::vector<std::string>& args);

}  // namespace pzt::cli::commands
