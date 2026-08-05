#pragma once

#include <string>
#include <vector>

// `pzt` 各子命令的入口。每个 cmd_* 接收 argv[2:] 的位置参数,返回进程退出
// 码。main() 只做子命令名分发,具体逻辑在 commands.cpp(小命令)和
// browse.cpp(`pzt open` 浏览主循环)里。
namespace pzt::cli::commands {

// 用错命令时打的 usage,走 stderr - 它是错误输出的一部分,不该混进被管道
// 消费的 stdout。用户主动求助请用 print_help()。
void print_usage();

// `pzt --help` / `pzt -h` / `pzt help`:同一份 usage,但走 stdout 且调用方
// 返回 0。求助不是用错,退出码必须是成功,输出也要能 `| less`、能重定向。
void print_help();

// `pzt --version` / `pzt version`:打印 PZT_VERSION 到 stdout。版本号是语言
// 无关的,不走 i18n。
void print_version();

int cmd_new(const std::vector<std::string>& args);
int cmd_list(const std::vector<std::string>& args);
int cmd_open(const std::vector<std::string>& args);
int cmd_archive(const std::vector<std::string>& args);
int cmd_unarchive(const std::vector<std::string>& args);
int cmd_delete(const std::vector<std::string>& args);
int cmd_rescan(const std::vector<std::string>& args);
int cmd_export(const std::vector<std::string>& args);
int cmd_tag(const std::vector<std::string>& args);
int cmd_recipe(const std::vector<std::string>& args);

// M4：headless 命令(JSON 进出，非交互)，供 agent/ 子进程调用，见
// docs/history/M4_Eng_Design.md"headless 命令面设计"一节。
int cmd_images(const std::vector<std::string>& args);
int cmd_dedup(const std::vector<std::string>& args);
int cmd_export_images(const std::vector<std::string>& args);
int cmd_eval(const std::vector<std::string>& args);
int cmd_curate(const std::vector<std::string>& args);
int cmd_compare(const std::vector<std::string>& args);

}  // namespace pzt::cli::commands
