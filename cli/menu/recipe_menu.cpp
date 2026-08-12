#include "cli/menu/recipe_menu.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cli/text/text.h"
#include "cli/ui/ui.h"
#include "cli/i18n/i18n.h"

// prompt_and_read_key / read_text_line 来自 cli/ui,用 using-directive 让
// 搬过来的函数体保持逐字不变(.cpp 里用 using,头文件里绝不用)。
using namespace pzt::cli::ui;

namespace pzt::cli::menu {
namespace {

// increment 6:`r` 前缀键完整交互。Origin 固定 id=0,不参与数字编号——
// r+0/r+r(清除)已经是独立于预设列表的快捷路径,直接把 recipe_id 设成
// NULL,不需要 Origin 本身占一个键位。过滤之后剩下的 9 个 City+Year 预
// 设按创建顺序(id 升序)映射到键 1-9,数量和顺序由
// core/recipe/recipe.cpp::ensure_default_presets 里的表决定,见
// docs/history/W2026-07-15_RecipeExpansion_Eng_Design.md。
std::vector<pzt::core::PresetSummary> presets_for_menu() {
  auto presets = pzt::core::list_presets();
  std::vector<pzt::core::PresetSummary> numbered;
  for (auto& p : presets) {
    if (p.id == 0) continue;
    numbered.push_back(std::move(p));
  }
  if (numbered.size() > 9) numbered.resize(9);
  return numbered;
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
//
// issue #19:9 个字段从一次性问完的线性问答改成可前进/后退的向导(导航循
// 环见 run_field_wizard)。"静默归零"这条哲学不变:有了字段级回退,填错了
// 直接退回去改,不需要再叠一层拒绝/重提示。
//
// issue #20:每提交一格就调一次 preview,把当前已知的完整参数组合套到正在
// 浏览的这张图上。preview 可以是空的(当前图片解码失败),那时向导退化成纯
// 文字流程、照常能建出 version。
std::string handle_r_create_flow(int banner_row, int start_col, int content_cols,
                                  const PreviewFn& preview) {
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

  // 9 个字段的提示文案,顺序即向导顺序,也即下面取值的下标顺序。前 8 格是
  // 数值(params_from_wizard_fields 只认这 8 格),最后一格是名字。
  const std::vector<std::string> prompts = {
      pzt::cli::i18n::recipe_menu_input_highlights(), pzt::cli::i18n::recipe_menu_input_shadows(),
      pzt::cli::i18n::recipe_menu_input_wb_r(),       pzt::cli::i18n::recipe_menu_input_wb_b(),
      pzt::cli::i18n::recipe_menu_input_contrast(),   pzt::cli::i18n::recipe_menu_input_saturation(),
      pzt::cli::i18n::recipe_menu_input_blacks(),     pzt::cli::i18n::recipe_menu_input_whites(),
      pzt::cli::i18n::recipe_menu_input_name()};

  auto values = run_field_wizard(
      prompts.size(), [&](std::size_t index, const std::string& current) {
        const bool can_go_back = index > 0;
        // "第几步/共几步 + 能不能退"是这个向导唯一的可发现性来源:Backspace
        // 回退没有任何视觉痕迹,不写在提示里用户不会知道它存在。
        std::string prompt =
            pzt::cli::i18n::recipe_menu_wizard_step_prefix(index + 1, prompts.size(), can_go_back) +
            prompts[index];
        // 第一个字段上关掉回退,而不是靠 run_field_wizard 那边"index 为 0
        // 就不动"兜着:那样这一格会被重新读一次,回填的是这个字段上次提交
        // 的值,用户刚删空的内容会原地长回来,那是"产生了效果",正是验收
        // 标准里说不该发生的事。两边都判一次不是重复:这里管的是这一格的
        // 按键语义,那边管的是导航不越界(读取方是参数,不保证不返回 Back)。
        return read_text_line_for_wizard(prompt, current, can_go_back, banner_row, start_col,
                                          content_cols);
      },
      // issue #20:每提交一格就把当前已知的完整组合套到正在浏览的这张图上重
      // 画一次。preview 为空(当前图片解码不出来)时向导照常走完,只是没有画
      // 面——预览是这个流程的辅助,不是它的前置条件。
      [&](const std::vector<std::string>& current_values) {
        if (preview) preview(preset->id, params_from_wizard_fields(current_values));
      });
  if (!values) return "";  // Esc 中止整个流程,不写入任何 version

  // 跟每一帧预览用的是同一份映射,不在这里重写一遍(见 params_from_wizard_fields)。
  pzt::core::VersionParams params = params_from_wizard_fields(*values);
  // 名字是唯一不进 VersionParams 的字段,只能在这里按下标取。写成常量而不是
  // 裸 8,是因为字段表哪天加一格的话,裸下标读到的会是错的那一格(或者越界),
  // 而两处都写 8 的时候编译器一句话都不会说。
  const std::size_t kNameFieldIndex = prompts.size() - 1;
  const std::string& name_text = (*values)[kNameFieldIndex];
  std::optional<std::string> name =
      name_text.empty() ? std::nullopt : std::optional<std::string>(name_text);

  auto result = pzt::core::create_version(preset->id, name, params);
  if (!result.ok()) return pzt::cli::i18n::recipe_menu_create_failed();
  return pzt::cli::i18n::recipe_menu_create_success(preset->name);
}

}  // namespace

// 向导的导航循环(契约见头文件)。values 全程按下标存"该字段最近一次提交
// 的值",回退时原样交回给读取方当初值,前进时也不清空后面已经填过的字
// 段:用户改的是前面某一格,后面填好的内容没有理由消失。
std::optional<std::vector<std::string>> run_field_wizard(
    std::size_t field_count,
    const std::function<pzt::cli::ui::WizardLineResult(std::size_t, const std::string&)>&
        read_field,
    const std::function<void(const std::vector<std::string>&)>& on_values_changed) {
  std::vector<std::string> values(field_count);
  std::size_t index = 0;
  while (index < field_count) {
    auto result = read_field(index, values[index]);
    switch (result.action) {
      case pzt::cli::ui::WizardLineAction::Cancelled:
        return std::nullopt;
      case pzt::cli::ui::WizardLineAction::Back:
        // 第一个字段没有上一格,停在原地重问,不是退出向导。
        if (index > 0) --index;
        break;
      case pzt::cli::ui::WizardLineAction::Submitted:
        values[index] = result.text;
        ++index;
        // 提交是唯一会改动 values 的分支,所以也是唯一需要通知的分支(见头
        // 文件里关于回退为什么不通知的说明)。
        if (on_values_changed) on_values_changed(values);
        break;
    }
  }
  return values;
}

pzt::core::VersionParams params_from_wizard_fields(const std::vector<std::string>& values) {
  auto parse_double_or_zero = [](const std::string& s) -> double {
    if (s.empty()) return 0.0;
    try {
      std::size_t consumed = 0;
      double v = std::stod(s, &consumed);
      if (consumed == s.size()) return v;
    } catch (const std::exception&) {
      // 解析失败,落到下面的 0.0
    }
    return 0.0;
  };
  // 向导中途 values 里靠后的字段还是空串,越界时同样按空串处理——两种情况
  // 归零的理由是同一个,不值得分开写。
  auto field = [&](std::size_t i) -> double {
    return i < values.size() ? parse_double_or_zero(values[i]) : 0.0;
  };

  pzt::core::VersionParams params;
  params.highlights = field(0);
  params.shadows = field(1);
  params.wb_shift_r = field(2);
  params.wb_shift_b = field(3);
  params.contrast = field(4);
  params.saturation = field(5);
  params.blacks = field(6);
  params.whites = field(7);
  return params;
}

// 预设一多，单行装不下——把编号选项按"整个 N:[名字] 不拆行"的原则铺到两
// 行(第一行装不下的整个单元挪到第二行)，操作图例(r/c/d/esc)右对齐贴在第二
// 行末尾。返回 {line1, line2} 交给 prompt_and_read_key_2line 渲染。两行都带
// 一个前导空格，跟其它 banner 的留白风格一致。presets 不含 Origin(见
// presets_for_menu)，这里在编号预设之后补一个纯展示用的 "0:[Origin]"——
// `r`+`0`/`r`+`r` 在 handle_r_key 里本来就是清除风格的快捷路径，只是之前
// 被 1735c40 连同数字编号一起过滤掉、菜单上看不到这个入口了，这里只补显
// 示，不改行为。
std::pair<std::string, std::string> build_recipe_menu_lines(
    const std::vector<pzt::core::PresetSummary>& presets, const std::string& legend,
    int content_cols) {
  const auto width = static_cast<std::size_t>(content_cols);
  const std::string sep = "  ";

  std::vector<std::string> items;
  items.reserve(presets.size() + 1);
  for (std::size_t i = 0; i < presets.size(); ++i) {
    items.push_back(pzt::cli::i18n::menu_item(std::to_string(i + 1), presets[i].name));
  }
  items.push_back(pzt::cli::i18n::menu_item("0", "Origin"));

  // 第一行:尽量多的完整 item。加上去会超宽就停,整个单元留给第二行。
  std::string line1 = " ";
  std::size_t idx = 0;
  for (; idx < items.size(); ++idx) {
    std::string piece = (idx == 0 ? std::string() : sep) + items[idx];
    if (pzt::cli::text::display_width(line1) + pzt::cli::text::display_width(piece) > width) break;
    line1 += piece;
  }

  // 第二行:剩余 item + 右对齐的操作图例。给图例预留宽度,不跟 item 抢。
  const std::size_t legend_w = pzt::cli::text::display_width(legend);
  std::string line2_items = " ";
  for (std::size_t j = idx; j < items.size(); ++j) {
    std::string piece = (j == idx ? std::string() : sep) + items[j];
    if (pzt::cli::text::display_width(line2_items) + pzt::cli::text::display_width(piece) + legend_w >
        width) {
      break;
    }
    line2_items += piece;
  }
  const std::size_t used = pzt::cli::text::display_width(line2_items) + legend_w;
  const std::size_t gap = used < width ? width - used : 0;
  std::string line2 = line2_items + std::string(gap, ' ') + legend;

  return {line1, line2};
}

RKeyOutcome handle_r_key(pzt::core::ImageId image_id, int banner_row, int start_col,
                         int content_cols, const PreviewFn& preview) {
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
    // 预设一多单行拼不下:编号选项按整个单元铺满两行(不拆 id 和名字)，操作
    // 图例右对齐贴第二行末尾,见 build_recipe_menu_lines。
    auto [line1, line2] =
        build_recipe_menu_lines(presets, pzt::cli::i18n::recipe_menu_actions_line(has_recipe),
                                 content_cols);
    char c = prompt_and_read_key_2line(line1, line2, banner_row, start_col, content_cols);
    if (c == 'r' || c == '0') {
      auto result = pzt::core::set_image_recipe(image_id, std::nullopt);
      if (!result.ok()) return {RKeyAction::Cancelled, pzt::cli::i18n::recipe_menu_clear_failed()};
      return {RKeyAction::Cleared, ""};
    }
    if (c == 'v' && has_recipe) {
      return {RKeyAction::Toggled, ""};
    }
    if (c == 'c') {
      std::string result = handle_r_create_flow(banner_row, start_col, content_cols, preview);
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
