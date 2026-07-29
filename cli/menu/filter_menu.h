#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/api.h"

// `f` 前缀键的筛选菜单。见 docs/M0_Eng_Design.md increment 6.4.6/6.6。
// 入口键最初是 `g`,后来改成 `f`(筛选/Filter 首字母);这一层的类型和函数
// 早期跟着旧键名叫 GKey*/handle_g_key_prompt,2026-07-29 一并改齐,历史
// 文档里出现的 `g + 数字` 说的都是今天的 `f + 数字`。
namespace pzt::cli::menu {

// increment 6.4.6:f + 数字切换到只浏览某个标签下图片的筛选视图,f + f
// 清除筛选。落地这个决定需要改 cmd_open 自己的 images/current_id/
// prefetch——这几个是 cmd_open 函数作用域内的局部变量,不像其它 handle_*
// 那样能靠传值参数完成整个动作,所以这里只返回"意图",不直接执行。
// 点 1：旧的 `g+e`"挑任意标签导出"这条路径已经退休——导出统一走顶层
// `e` 键,筛选菜单只剩筛选/清除筛选两种意图,不再需要 Handled 这个"已经
// 在内部执行完"的分支。
enum class FKeyAction { Cancel, ClearFilter, ApplyFilter };

struct FKeyDecision {
  FKeyAction action = FKeyAction::Cancel;
  pzt::core::TagId tag_id{};  // 只有 action == ApplyFilter 时有意义
  std::string tag_name;       // 同上,顺便带出来给信息栏筛选提示用,不用每
                              // 帧再查一次 tags_for_menu 只为了显示名字
  // action == Cancel 时,Esc 取消是空字符串(静默),按了个不认识的键则带
  // 一句"无效按键"提示(跟 handle_r_key 保持一致)。action == ApplyFilter/
  // ClearFilter 时没有意义,恒为空。
  std::string status;
};

// f 键入口:显示筛选/清除选项,返回一个"意图"(FKeyDecision)交给
// cmd_open 执行。tags 由调用方(cmd_open)用 tags_for_menu 构好后传入。
// F-01：duplicate_tag_id 为空表示项目还没有"重复"系统标签,`9` 不出现
// 在菜单里、按了也不响应,跟 handle_space_key 同样的处理方式。
FKeyDecision handle_f_key_prompt(pzt::core::TagId reject_tag_id,
                                 std::optional<pzt::core::TagId> duplicate_tag_id,
                                 const std::vector<pzt::core::TagSummary>& tags, int banner_row,
                                 int start_col, int content_cols);

}  // namespace pzt::cli::menu
