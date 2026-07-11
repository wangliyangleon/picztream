#include "cli/menu/tag_menu.h"

#include <algorithm>
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

// cap 超限时,在 banner 那一行显示已有条目、读一个键选替换对象或取消。
std::string handle_cap_replace_submenu(pzt::core::TagId tag_id, pzt::core::ImageId new_image_id,
                                        const pzt::core::CapExceededInfo& cap_info, int banner_row,
                                        int start_col, int content_cols) {
  if (cap_info.existing_entries.empty()) {
    return pzt::cli::i18n::tag_menu_cap_zero();
  }
  std::size_t shown = std::min<std::size_t>(cap_info.existing_entries.size(), 9);

  std::string line = pzt::cli::i18n::tag_menu_full(static_cast<int>(cap_info.cap));
  for (std::size_t i = 0; i < shown; ++i) {
    if (i > 0) line += "  ";
    line += pzt::cli::i18n::menu_item(std::to_string(i + 1), cap_info.existing_entries[i].file_name);
  }
  line += pzt::cli::i18n::tag_menu_esc_cancel();
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c < '1' || c > static_cast<char>('0' + shown)) return "";  // 取消,静默

  const auto& old_entry = cap_info.existing_entries[static_cast<std::size_t>(c - '1')];
  auto result = pzt::core::replace_tag_entry(tag_id, old_entry.image_id, new_image_id);
  if (!result.ok()) return pzt::cli::i18n::tag_menu_replace_failed();
  return pzt::cli::i18n::tag_menu_replaced(old_entry.file_name);
}

// increment 6.4.4.5:space - 摘除标签。复用跟"加标签"完全相同的列表/编
// 号(id 升序,不是"只显示当前图片有的标签")——`tags_for_menu` 客户端重
// 排序就是为了让"数字 N 永远对应同一个标签"这条肌肉记忆成立,摘除菜单换
// 一套编号的话,同一个数字在两个菜单里意味着不同标签,反而更容易按错。选
// 中一个当前图片本来就没有的标签,`remove_tag` 本来就是幂等的,不需要额
// 外校验或过滤。菜单文案前缀"摘除:"跟加标签菜单区分开,给一个明确的视觉
// 提示"现在是摘除模式"。
std::string handle_remove_tag_submenu(const std::vector<pzt::core::TagSummary>& tags,
                                       pzt::core::TagId reject_tag_id,
                                       std::optional<pzt::core::TagId> duplicate_tag_id,
                                       pzt::core::ImageId image_id, int banner_row, int start_col,
                                       int content_cols) {
  std::string line = pzt::cli::i18n::tag_menu_remove_prefix();
  for (std::size_t i = 0; i < tags.size(); ++i) {
    line += "  " + pzt::cli::i18n::menu_item(std::to_string(i + 1), tags[i].name);
  }
  // F-01：`9:重复` 只在项目里已经存在这个系统标签时才出现在摘除菜单
  // 里，不强迫用户面对一个从没跑过 /dedup 的项目也看到这个选项。
  if (duplicate_tag_id) {
    line += "  " + pzt::cli::i18n::menu_item("9", pzt::cli::i18n::duplicate_tag_label());
  }
  line += pzt::cli::i18n::tag_menu_esc_cancel();
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  pzt::core::TagId tag_id;
  if (c == '0') {
    tag_id = reject_tag_id;
  } else if (c == '9' && duplicate_tag_id) {
    tag_id = *duplicate_tag_id;
  } else if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
    tag_id = tags[static_cast<std::size_t>(c - '1')].id;
  } else {
    return "";  // 取消,静默
  }

  auto result = pzt::core::remove_tag(image_id, tag_id);
  if (!result.ok()) return pzt::cli::i18n::tag_menu_remove_failed();  // 防御性,理论上不应该发生
  return "";  // 静默成功,信息栏下一帧自然显示摘除后的结果
}

// increment 6.4.4.5:space c 新建标签。跟其它菜单不同,这里需要读入一个
// 完整的标签名(多字符文本),用 read_text_line;上限和是否排序两个后续问
// 题分别用文本输入/单字节是否处理。
std::string handle_create_tag_flow(pzt::core::ProjectId project_id, int banner_row, int start_col,
                                    int content_cols) {
  auto name = read_text_line(pzt::cli::i18n::tag_menu_new_name_prompt(), banner_row, start_col, content_cols);
  if (!name) return "";  // Esc,静默取消
  if (name->empty()) {
    // 空回车不是 Esc——用户确实按了键,不能像 Esc 一样什么反馈都没有。
    return pzt::cli::i18n::tag_menu_new_name_empty();
  }

  auto cap_text =
      read_text_line(pzt::cli::i18n::tag_menu_cap_prompt(), banner_row, start_col, content_cols);
  if (!cap_text) return "";  // Esc 中止整个流程,即便名字已经输完了
  std::optional<std::int64_t> cap;
  if (!cap_text->empty()) {
    // 尝试整体解析成正整数;解析不完整/非数字/<=0 都当"不限"处理,不重新
    // 提示、不阻塞重试——cap 是低风险的元数据(填错了大不了删掉重建),这
    // 个菜单一贯的风格是不做校验重试循环。
    try {
      std::size_t consumed = 0;
      long long parsed = std::stoll(*cap_text, &consumed);
      if (consumed == cap_text->size() && parsed > 0) {
        cap = parsed;
      }
    } catch (const std::exception&) {
      // 解析失败,cap 保持 nullopt(不限)
    }
  }

  char c = prompt_and_read_key(pzt::cli::i18n::tag_menu_order_prompt() +
                                pzt::cli::i18n::tag_menu_ordered_keys_help(),
                                banner_row, start_col, content_cols);
  if (c == 0x1B) return "";  // Esc 在这一步依然中止整个流程
  bool is_ordered = (c == 'y' || c == 'Y');  // 其它任何键(包括裸回车)都算"否"

  auto result = pzt::core::create_tag(project_id, *name, cap, is_ordered);
  if (!result.ok()) return pzt::cli::i18n::tag_menu_name_exists(*name);
  return pzt::cli::i18n::tag_menu_created(*name);
}

// increment 6.4.4.5:space d 删除标签定义本身(项目级、级联清除所有图片
// 与它的关联),不是"摘掉当前图片的标签"。只列非系统标签——系统标签(目
// 前只有还没落地的"废片")不能删除,从一开始就不给选,而不是选了之后再拒
// 绝。比菜单里其它操作多一次按键确认("y" 才执行),因为这是这个菜单里破
// 坏性最强、唯一项目级且不可逆的操作,但没有重到 `pzt delete` 那种"重新打
// 一遍项目名"的程度——那种量级的确认是给独立执行的破坏性命令用的,跟这
// 个强调"不打断心流"的菜单体系不搭。
std::string handle_delete_tag_submenu(const std::vector<pzt::core::TagSummary>& tags,
                                       int banner_row, int start_col, int content_cols) {
  std::vector<pzt::core::TagSummary> deletable;
  for (const auto& t : tags) {
    if (!t.is_system) deletable.push_back(t);
  }
  if (deletable.empty()) {
    return pzt::cli::i18n::tag_menu_no_deletable();
  }

  std::string line = pzt::cli::i18n::tag_menu_delete_prefix();
  for (std::size_t i = 0; i < deletable.size(); ++i) {
    if (i > 0) line += "  ";
    line += pzt::cli::i18n::tag_menu_delete_item(static_cast<int>(i + 1), deletable[i].name,
                                                  deletable[i].tagged_count);
  }
  line += pzt::cli::i18n::tag_menu_esc_cancel();
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c < '1' || c > static_cast<char>('0' + deletable.size())) return "";  // 取消,静默
  const auto& chosen = deletable[static_cast<std::size_t>(c - '1')];

  std::string confirm = pzt::cli::i18n::tag_menu_delete_confirm(chosen.name, chosen.tagged_count);
  char yn = prompt_and_read_key(confirm, banner_row, start_col, content_cols);
  if (yn != 'y' && yn != 'Y') return "";  // 取消,静默

  auto result = pzt::core::delete_tag(chosen.id);
  if (!result.ok()) return pzt::cli::i18n::tag_menu_delete_failed();
  return pzt::cli::i18n::tag_menu_deleted(chosen.name);
}

}  // namespace

// `list_tags` 是按名字字母序排的(给 `pzt tag list` 这类展示用)——数字键
// 要按标签创建顺序(tag id 升序)固定,不然新建一个名字靠前的标签会让所有
// 已有标签的数字悄悄错位。不改 `list_tags` 本身,这里对结果客户端重排序。
// increment 6.4.5:系统标签("废片")固定占硬编码的 `0`,不参与这个动态序
// 列,先过滤掉。F-01:动态标签只截断到 8 个(不是 9)——数字 `9` 现在固
// 定留给"重复"系统标签(见 handle_space_key/handle_g_key_prompt 里
// duplicate_tag_id 参数的用法),动态列表不能再占用它。
std::vector<pzt::core::TagSummary> tags_for_menu(pzt::core::ProjectId project_id) {
  auto tags = pzt::core::list_tags(project_id);
  tags.erase(std::remove_if(tags.begin(), tags.end(), [](const auto& t) { return t.is_system; }),
             tags.end());
  std::sort(tags.begin(), tags.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  if (tags.size() > 8) tags.resize(8);
  return tags;
}

// increment 6.4.5:数字加标签的分支和 `x` 快捷键共用同一段结果处理逻
// 辑,避免"处理 add_tag 结果、cap 超限转 handle_cap_replace_submenu"这
// 段逻辑抄两遍。`x` 的 cap 超限分支结构上不可能被真正触发(`废片` 的 cap
// 恒为空),复用这段逻辑单纯是不想把同一段代码写两份,不是专门防御性设计。
std::string handle_add_tag_result(pzt::core::TagId tag_id, pzt::core::ImageId image_id,
                                   int banner_row, int start_col, int content_cols) {
  auto result = pzt::core::add_tag(image_id, tag_id);
  if (result.ok()) return "";  // 静默成功,信息栏下一帧自然显示新标签

  const auto& err = result.error();
  if (err.kind == pzt::core::AddTagFailureKind::CapExceeded) {
    return handle_cap_replace_submenu(tag_id, image_id, *err.cap_info, banner_row, start_col,
                                       content_cols);
  }
  // TagNotFound/ImageNotFound/ProjectMismatch:tag_id/image_id 都来自刚查
  // 出来的数据,理论上不会发生,防御性处理而不是假设不可能。
  return pzt::cli::i18n::tag_menu_add_failed();
}

// space 键的入口:在 banner 那一行显示可选标签、读一个键选标签或取消,
// cap 超限时转入 handle_cap_replace_submenu;`-`/`c`/`d` 分别转入摘除/新
// 建/删除标签定义。`0:废片` 是硬编码的固定选项,不占用 `tags_for_menu` 的
// 动态 1-8 序列——`废片` 从 pzt new 起就保证存在,不再有"这次按 space 完
// 全没有东西可选"的情况,不管动态列表是不是空都无条件阻塞读一个键。F-01：
// `9:重复` 是另一个硬编码固定选项,跟 `0:废片` 对称,但只在 duplicate_
// tag_id 有值(项目已经跑过至少一次 /dedup)时才出现——不像废片那样保证
// 存在,调用方(cmd_open)每次按 space 前都重新查一次,不缓存。
std::string handle_space_key(pzt::core::ProjectId project_id, pzt::core::TagId reject_tag_id,
                              std::optional<pzt::core::TagId> duplicate_tag_id,
                              pzt::core::ImageId image_id, int banner_row, int start_col,
                              int content_cols) {
  // `c` 新建标签之后留在这个循环里,不管成功/失败/中途 Esc 取消都回到标签
  // 列表重新显示(新建成功的话,tags_for_menu 下一轮就能查到、直接出现在
  // 可选列表里)——用户建完一个新标签,大概率是想紧接着把它打上去,不该
  // 被退回一级菜单还得重新按一次 space。其它分支(加/摘/删标签)维持原
  // 样,做完就返回,不留在这个循环里。
  while (true) {
    auto tags = tags_for_menu(project_id);  // 每轮重新查,加/摘/删三个分支共用

    // 标签一多,单行拼不下,拆成两行:第一行编号选项,第二行固定字母操
    // 作,见 prompt_and_read_key_2line 的说明。
    char c = prompt_and_read_key_2line(
        pzt::cli::i18n::tag_menu_options_line(tags, duplicate_tag_id.has_value()),
        pzt::cli::i18n::tag_menu_actions_line(), banner_row, start_col, content_cols);
    if (c == 'c') {
      std::string result = handle_create_tag_flow(project_id, banner_row, start_col, content_cols);
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
    if (c == '-') {
      return handle_remove_tag_submenu(tags, reject_tag_id, duplicate_tag_id, image_id, banner_row,
                                        start_col, content_cols);
    }
    if (c == 'd') return handle_delete_tag_submenu(tags, banner_row, start_col, content_cols);
    if (c == '0') {
      return handle_add_tag_result(reject_tag_id, image_id, banner_row, start_col, content_cols);
    }
    if (c == '9' && duplicate_tag_id) {
      return handle_add_tag_result(*duplicate_tag_id, image_id, banner_row, start_col, content_cols);
    }
    if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
      const auto& chosen = tags[static_cast<std::size_t>(c - '1')];
      return handle_add_tag_result(chosen.id, image_id, banner_row, start_col, content_cols);
    }
    if (c == 0x1B) return "";  // Esc,静默
    // 不是 Esc,也不对应任何选项——跟 handle_r_key 一致,给一句反馈而不是完
    // 全没反应(真机反馈:直接退回一级菜单,分不清是没按对还是没反应)。
    return pzt::cli::i18n::recipe_menu_invalid_key();
  }
}

}  // namespace pzt::cli::menu
