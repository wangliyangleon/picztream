#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/api.h"

// `space` 键的标签菜单交互(加/摘/新建/删除标签)。见 docs/history/M0_Eng_Design.md
// increment 6.4.4。
namespace pzt::cli::menu {

// 动态标签在 space/f 菜单里用单个数字键 1-8 寻址(0 固定给"废片"、9 固定
// 给"重复",见 F-01),所以菜单一次最多认 8 个动态标签。这不是 core 的业务
// 规则(core 不限标签数量,`pzt tag list` 看得到全部、`pzt export` 用得了全
// 部),纯粹是"一个数字键对应一个选项"这个交互设计带来的输入端约束,所以上
// 限定在 cli 而不是 core,跟 recipe_menu 里 version 的 9 个上限同一条理由。
inline constexpr std::size_t kMaxMenuTags = 8;

// space 菜单与 `f` 筛选菜单共用的一套编号,以及这个项目的标签数相对上限的
// 位置。T-24 之前这里只返回截断后的列表、超出的部分被静默丢掉:第 9 个标
// 签在 space 打标签、space - 摘除、f 筛选三处全部不存在,而 `pzt tag list`
// 还看得到它、`pzt export` 还用得了它。现在超限信息跟着列表一起出去:建到
// 上限由 handle_space_key 的 `c` 分支挡住,已经超限的项目由菜单行尾的注记
// 说明,两条路都不再静默。
struct MenuTags {
  // 过滤掉系统标签、按 tag id 升序固定编号、截断到 kMaxMenuTags。
  std::vector<pzt::core::TagSummary> shown;
  // 超出上限、菜单里选不到的动态标签数量。两种情况会非 0:本次改动之前就
  // 建出 8 个以上标签的老项目;以及 headless 侧继续建出来的新标签-
  // `resolve_or_create_tag`(cli/commands/commands.cpp)给 `pzt tag apply`
  // 与 `pzt curate --apply-tag` 惰性建标签,不受这个上限约束,而 agent 的
  // Plan 里标签名是每次会话由 LLM 组装的。那条路故意不挡:headless 没有可
  // 以当场问的人,拒绝会让整个 agent 会话失败,而代价只是这个标签在 TUI 菜
  // 单里选不到-它照样能被 apply、被 export。
  std::size_t hidden = 0;
  // 动态标签数已达上限,不能再建。注意它跟 hidden > 0 不等价:数量正好等
  // 于上限时 at_limit 为真而 hidden 为 0。
  bool at_limit = false;
};

MenuTags tags_for_menu(pzt::core::ProjectId project_id);

// 处理 add_tag 结果:成功静默;cap 超限转入替换子菜单。数字加标签分支和
// `x` 快捷键共用这段逻辑,所以是 public(cmd_open 的 `x` 直接调它)。
std::string handle_add_tag_result(pzt::core::TagId tag_id, pzt::core::ImageId image_id,
                                  int banner_row, int start_col, int content_cols);

// space 键的入口:显示可选标签、读一个键选标签或转入 -/c/d 子流程。
// F-01：duplicate_tag_id 为空表示项目还没有"重复"系统标签(没跑过
// /dedup),这种情况下 `9` 不出现在菜单里、按了也不响应；调用方
// (cmd_open)每次按 space 前都重新查一次,不缓存(dedup 可能在同一次
// 浏览会话里第一次创建这个标签)。
std::string handle_space_key(pzt::core::ProjectId project_id, pzt::core::TagId reject_tag_id,
                             std::optional<pzt::core::TagId> duplicate_tag_id,
                             pzt::core::ImageId image_id, int banner_row, int start_col,
                             int content_cols);

}  // namespace pzt::cli::menu
