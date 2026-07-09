#include "cli/menu/recipe_menu.h"

#include <optional>
#include <string>
#include <vector>

#include "cli/ui/ui.h"
#include "cli/i18n/i18n.h"

// prompt_and_read_key / read_text_line 来自 cli/ui,用 using-directive 让
// 搬过来的函数体保持逐字不变(.cpp 里用 using,头文件里绝不用)。
using namespace pzt::cli::ui;

namespace pzt::cli::menu {
namespace {

// increment 6:`r` 前缀键完整交互。预设列表不需要像 tags_for_menu 那样过
// 滤 is_system——目前所有预设(包括 Origin)都是 is_system,但这里没有"用
// 户自建预设"这个对立面需要排除,直接按创建顺序编号 1-9。跟废片固定占 0
// 号位不同,Origin 没有固定编号,它就是 list_presets() 里排第一的普通一
// 项——`r` + `0`/`r` + `r`(清除)是完全独立于预设列表的快捷路径,直接把
// recipe_id 设成 NULL,不经过"选中 Origin 预设"这条路,两者对"没有风格"
// 这件事产出相同效果但走的是不同路径,见 core/recipe/recipe.h
// ensure_default_presets 的说明。
std::vector<pzt::core::PresetSummary> presets_for_menu() {
  auto presets = pzt::core::list_presets();
  if (presets.size() > 9) presets.resize(9);
  return presets;
}

// 应用/删除/新建三个流程都需要"这个预设下未软删除的 version"这份列表,
// 且都要截断到 9 个(单个数字键 1-9 的寻址上限)。截断之后的 size() 天然
// 等价于"原始未软删除数量是否 >= 9",所以只需要检查数量上限的调用方
// (handle_r_create_flow)不需要额外单独查一次未截断的计数,直接复用这
// 个函数就够了。
std::vector<pzt::core::VersionSummary> live_versions_for_menu(pzt::core::RecipeId preset_id) {
  auto all_versions = pzt::core::list_versions(preset_id);
  std::vector<pzt::core::VersionSummary> live;
  for (const auto& v : all_versions) {
    if (!v.deleted) live.push_back(v);
  }
  if (live.size() > 9) live.resize(9);
  return live;
}

// apply/create/delete 三个流程都要"选一个预设"，这是第三处需要这个交互
// 的地方(前两处是 cli 调试命令时代的 find_preset_by_name，这次是真正的
// 交互式菜单)，抽成共用函数。区分 Esc(真的想取消,静默)和"按了个不对应
// 任何预设的键"(真机测试反馈:这种情况应该有反馈,不能什么都不说——用户
// 分不清是"我没按对"还是"程序没反应")——用 out_message 带一句"该预设不
// 存在"出去，调用方只在它非空时才展示成状态提示。
std::optional<pzt::core::PresetSummary> handle_pick_preset_prompt(int banner_row, int start_col,
                                                                    int content_cols,
                                                                    std::string* out_message) {
  auto presets = presets_for_menu();
  std::string line = pzt::cli::i18n::recipe_menu_select_preset_prefix();
  for (std::size_t i = 0; i < presets.size(); ++i) {
    line += "  " + pzt::cli::i18n::menu_item(std::to_string(i + 1), presets[i].name);
  }
  line += pzt::cli::i18n::tag_menu_esc_cancel();
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c == 0x1B) return std::nullopt;  // Esc,静默
  if (c < '1' || c > static_cast<char>('0' + presets.size())) {
    *out_message = pzt::cli::i18n::recipe_menu_preset_not_exist();
    return std::nullopt;
  }
  return presets[static_cast<std::size_t>(c - '1')];
}

// 已经选定一个预设之后，第二层选"这个预设的中性状态(0,叫"默认",不是
// "预设本身"这种拗口的说法)"还是"某个已保存的 version(1-9)"——只列未软
// 删除的 version，编号规则跟 pzt recipe list/rename/delete 的寻址编号
// 一致(排除已删除的、按 id 升序排位)。同样区分 Esc 和无效按键，见
// handle_pick_preset_prompt 的说明。
std::optional<pzt::core::RecipeId> handle_pick_version_to_apply_prompt(
    const pzt::core::PresetSummary& preset, int banner_row, int start_col, int content_cols,
    std::string* out_message) {
  auto live = live_versions_for_menu(preset.id);

  std::string line = pzt::cli::i18n::recipe_menu_version_prompt(preset.name);
  for (std::size_t i = 0; i < live.size(); ++i) {
    line += "  " + pzt::cli::i18n::menu_item(
                       std::to_string(i + 1),
                       live[i].name.value_or(pzt::cli::i18n::recipe_menu_version_default_label()));
  }
  line += pzt::cli::i18n::tag_menu_esc_cancel();
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c == 0x1B) return std::nullopt;  // Esc,静默
  if (c == '0') return preset.id;
  if (c >= '1' && c <= static_cast<char>('0' + live.size())) {
    return live[static_cast<std::size_t>(c - '1')].id;
  }
  *out_message = pzt::cli::i18n::recipe_menu_preset_not_exist();
  return std::nullopt;
}

// 删除流程的第二层选择：跟应用流程共用同一份"这个预设下未删除的
// version"列表，但不提供"0:默认"这个选项——预设不可删除，从一开始就不
// 给选，不是"选了之后拒绝"。
std::string handle_pick_version_to_delete_prompt(const pzt::core::PresetSummary& preset,
                                                  int banner_row, int start_col,
                                                  int content_cols) {
  auto live = live_versions_for_menu(preset.id);
  if (live.empty()) {
    return pzt::cli::i18n::recipe_menu_no_deletable_versions(preset.name);
  }

  std::string line = pzt::cli::i18n::recipe_menu_delete_version_prefix(preset.name);
  for (std::size_t i = 0; i < live.size(); ++i) {
    line += "  " + pzt::cli::i18n::menu_item(
                       std::to_string(i + 1),
                       live[i].name.value_or(pzt::cli::i18n::recipe_menu_version_default_label()));
  }
  line += pzt::cli::i18n::tag_menu_esc_cancel();
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c == 0x1B) return "";  // Esc,静默
  if (c < '1' || c > static_cast<char>('0' + live.size())) return pzt::cli::i18n::recipe_menu_preset_not_exist();

  const auto& chosen = live[static_cast<std::size_t>(c - '1')];
  // 软删除:不影响已经引用这个 version 的图片渲染，只是从这个菜单里消
  // 失。跟 handle_delete_tag_submenu 的硬删除不同，这里不加一道额外的
  // y/N 二次确认——标签删除是级联清掉所有图片关联、不可逆的项目级操
  // 作，这里只是把它从"可选列表"里隐藏，风险量级不一样，不需要同等重量
  // 的确认仪式。
  auto result = pzt::core::delete_version(chosen.id);
  if (!result.ok()) return pzt::cli::i18n::recipe_menu_delete_failed();
  return pzt::cli::i18n::recipe_menu_delete_success(chosen.name.value_or(pzt::cli::i18n::recipe_menu_version_default_label()));
}

// increment 6.2:`r c` 交互式创建新 version——选一个基础预设、依次读高
// 光/暗光/白平衡红/蓝几个数值、可选的名字，对齐 handle_create_tag_flow
// 的多步骤读取风格。数值解析失败/留空都当 0 处理，不重新提示、不阻塞重
// 试——这几个参数是低风险的元数据，填错了大不了删掉重建，跟标签 cap 解
// 析失败时的处理哲学一致。创建之后不会自动应用到当前图片，对齐
// `space c` 建标签之后也不会自动打到当前图片上这个既有约定。
std::string handle_r_create_flow(int banner_row, int start_col, int content_cols) {
  std::string message;
  auto preset = handle_pick_preset_prompt(banner_row, start_col, content_cols, &message);
  if (!preset) return message;  // Esc 时 message 是空的,静默；无效选择时带一句反馈

  // 应用/删除菜单的第二层用单个数字 1-9 寻址 version,一个预设下超过 9
  // 个未删除的 version 就没有按键能选中它们——这不是 create_version 本
  // 身的业务规则(core 层不设上限,pzt recipe list/rename/delete 不受这
  // 个限制),纯粹是交互菜单"一个数字键对应一个选项"这个设计决定带来的
  // 输入端约束,所以检查放在这里而不是 core 里。live_versions_for_menu
  // 本身就截断到 9,截断后的 size() 达到 9 等价于"原始数量 >= 9"。
  if (live_versions_for_menu(preset->id).size() >= 9) {
    return pzt::cli::i18n::recipe_menu_custom_full(preset->name);
  }

  auto parse_double_or_zero = [](const std::optional<std::string>& s) -> double {
    if (!s || s->empty()) return 0.0;
    try {
      std::size_t consumed = 0;
      double v = std::stod(*s, &consumed);
      if (consumed == s->size()) return v;
    } catch (const std::exception&) {
      // 解析失败,落到下面的 0.0
    }
    return 0.0;
  };

  auto highlights_text =
      read_text_line(pzt::cli::i18n::recipe_menu_input_highlights(), banner_row, start_col, content_cols);
  if (!highlights_text) return "";  // Esc 中止整个流程
  auto shadows_text = read_text_line(pzt::cli::i18n::recipe_menu_input_shadows(), banner_row, start_col, content_cols);
  if (!shadows_text) return "";
  auto wb_r_text =
      read_text_line(pzt::cli::i18n::recipe_menu_input_wb_r(), banner_row, start_col, content_cols);
  if (!wb_r_text) return "";
  auto wb_b_text =
      read_text_line(pzt::cli::i18n::recipe_menu_input_wb_b(), banner_row, start_col, content_cols);
  if (!wb_b_text) return "";
  auto name_text =
      read_text_line(pzt::cli::i18n::recipe_menu_input_name(), banner_row, start_col, content_cols);
  if (!name_text) return "";

  pzt::core::VersionParams params;
  params.highlights = parse_double_or_zero(highlights_text);
  params.shadows = parse_double_or_zero(shadows_text);
  params.wb_shift_r = parse_double_or_zero(wb_r_text);
  params.wb_shift_b = parse_double_or_zero(wb_b_text);
  std::optional<std::string> name = name_text->empty() ? std::nullopt : name_text;

  auto result = pzt::core::create_version(preset->id, name, params);
  if (!result.ok()) return pzt::cli::i18n::recipe_menu_create_failed();
  return pzt::cli::i18n::recipe_menu_create_success(preset->name);
}

}  // namespace

RKeyOutcome handle_r_key(pzt::core::ImageId image_id, int banner_row, int start_col,
                         int content_cols) {
  // `c` 新建 version 之后留在这个循环里,不管成功/失败/中途 Esc 取消都回
  // 到预设列表重新显示(跟 handle_space_key 的 `c` 分支同一个理由:建完
  // 一个新 version,大概率是想紧接着把它应用上去,不该被退回一级菜单)。
  // 其它分支(应用/清除/切换/删除)维持原样,做完就返回,不留在这个循环
  // 里。
  while (true) {
    auto presets = presets_for_menu();
    // `v`(原图/风格化切换)只在这张图确实应用了风格时才有意义、才显示这
    // 个选项——没有风格可言时,切换没有任何视觉效果,不该占一个选项误导
    // 用户。文案固定写"切换原图/风格化",不再跟着 show_original 动态变
    // (之前试过跟着状态变文案,反而更难读)。
    bool has_recipe = pzt::core::get_image_recipe(image_id).has_value();
    // 预设一多,单行拼不下,拆成两行:第一行编号选项,第二行固定字母操
    // 作,见 prompt_and_read_key_2line 的说明。
    char c = prompt_and_read_key_2line(pzt::cli::i18n::recipe_menu_options_line(presets),
                                        pzt::cli::i18n::recipe_menu_actions_line(has_recipe),
                                        banner_row, start_col, content_cols);
    if (c == 'r' || c == '0') {
      auto result = pzt::core::set_image_recipe(image_id, std::nullopt);
      if (!result.ok()) return {RKeyAction::Cancelled, pzt::cli::i18n::recipe_menu_clear_failed()};
      return {RKeyAction::Cleared, ""};
    }
    if (c == 'v' && has_recipe) {
      return {RKeyAction::Toggled, ""};
    }
    if (c == 'c') {
      std::string result = handle_r_create_flow(banner_row, start_col, content_cols);
      if (!result.empty()) {
        // 跟 cmd_open 里 status_override 的处理逻辑一致:消息自带尾随空
        // 格,先去掉再拼"，按任意键继续"，不然中间会留一大段空白。
        std::string trimmed = result;
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        prompt_and_read_key(pzt::cli::i18n::msg_press_any_key_to_continue(trimmed), banner_row,
                             start_col, content_cols);
      }
      continue;
    }
    if (c == 'd') {
      std::string message;
      auto preset = handle_pick_preset_prompt(banner_row, start_col, content_cols, &message);
      if (!preset) return {RKeyAction::Cancelled, message};
      return {RKeyAction::Handled,
              handle_pick_version_to_delete_prompt(*preset, banner_row, start_col, content_cols)};
    }
    if (c == 0x1B) return {RKeyAction::Cancelled, ""};  // Esc,静默
    if (c >= '1' && c <= static_cast<char>('0' + presets.size())) {
      const auto& preset = presets[static_cast<std::size_t>(c - '1')];
      std::string message;
      auto recipe_id = handle_pick_version_to_apply_prompt(preset, banner_row, start_col,
                                                            content_cols, &message);
      if (!recipe_id) return {RKeyAction::Cancelled, message};
      auto result = pzt::core::set_image_recipe(image_id, *recipe_id);
      if (!result.ok()) return {RKeyAction::Cancelled, pzt::cli::i18n::recipe_menu_apply_failed()};
      return {RKeyAction::Applied, ""};
    }
    // 不是 Esc,也不对应任何选项(比如按了个字母、或者超出预设编号范围)——
    // 跟上面几个子菜单一致,给一句反馈而不是完全没反应。
    return {RKeyAction::Cancelled, pzt::cli::i18n::recipe_menu_invalid_key()};
  }
}

}  // namespace pzt::cli::menu
