#include "cli/menu/tag_menu.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "cli/ui/ui.h"

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
    return " 标签上限为 0,无法添加 ";  // cap=0 的极端配置,没有可替换的条目
  }
  std::size_t shown = std::min<std::size_t>(cap_info.existing_entries.size(), 9);

  std::string line = " 已满(" + std::to_string(cap_info.cap) + "):";
  for (std::size_t i = 0; i < shown; ++i) {
    if (i > 0) line += "  ";
    line += std::to_string(i + 1) + ":" + cap_info.existing_entries[i].file_name;
  }
  line += "  Esc 取消";
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c < '1' || c > static_cast<char>('0' + shown)) return "";  // 取消,静默

  const auto& old_entry = cap_info.existing_entries[static_cast<std::size_t>(c - '1')];
  auto result = pzt::core::replace_tag_entry(tag_id, old_entry.image_id, new_image_id);
  if (!result.ok()) return " 替换失败,请重试 ";  // 防御性,理论上不应该发生
  return " 已替换 '" + old_entry.file_name + "' ";
}

// increment 6.4.4.5:space - 摘除标签。复用跟"加标签"完全相同的列表/编
// 号(id 升序,不是"只显示当前图片有的标签")——`tags_for_menu` 客户端重
// 排序就是为了让"数字 N 永远对应同一个标签"这条肌肉记忆成立,摘除菜单换
// 一套编号的话,同一个数字在两个菜单里意味着不同标签,反而更容易按错。选
// 中一个当前图片本来就没有的标签,`remove_tag` 本来就是幂等的,不需要额
// 外校验或过滤。菜单文案前缀"摘除:"跟加标签菜单区分开,给一个明确的视觉
// 提示"现在是摘除模式"。
std::string handle_remove_tag_submenu(const std::vector<pzt::core::TagSummary>& tags,
                                       pzt::core::TagId reject_tag_id, pzt::core::ImageId image_id,
                                       int banner_row, int start_col, int content_cols) {
  std::string line = " 摘除:0:废片";
  for (std::size_t i = 0; i < tags.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + tags[i].name;
  }
  line += "  Esc 取消";
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  pzt::core::TagId tag_id;
  if (c == '0') {
    tag_id = reject_tag_id;
  } else if (c >= '1' && c <= static_cast<char>('0' + tags.size())) {
    tag_id = tags[static_cast<std::size_t>(c - '1')].id;
  } else {
    return "";  // 取消,静默
  }

  auto result = pzt::core::remove_tag(image_id, tag_id);
  if (!result.ok()) return " 摘标签失败,请重试 ";  // 防御性,理论上不应该发生
  return "";  // 静默成功,信息栏下一帧自然显示摘除后的结果
}

// increment 6.4.4.5:space c 新建标签。跟其它菜单不同,这里需要读入一个
// 完整的标签名(多字符文本),用 read_text_line;上限和是否排序两个后续问
// 题分别用文本输入/单字节是否处理。
std::string handle_create_tag_flow(pzt::core::ProjectId project_id, int banner_row, int start_col,
                                    int content_cols) {
  auto name = read_text_line(" 新标签名称: ", banner_row, start_col, content_cols);
  if (!name) return "";  // Esc,静默取消
  if (name->empty()) {
    // 空回车不是 Esc——用户确实按了键,不能像 Esc 一样什么反馈都没有。
    return " 标签名不能为空,已取消 ";
  }

  auto cap_text =
      read_text_line(" 上限数量(直接 Enter = 不限): ", banner_row, start_col, content_cols);
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

  char c = prompt_and_read_key(" 是否需要按顺序排列(用于朋友圈九宫格等,直接 Enter = 否): "
                                "y 是 / 其它键 = 否 ",
                                banner_row, start_col, content_cols);
  if (c == 0x1B) return "";  // Esc 在这一步依然中止整个流程
  bool is_ordered = (c == 'y' || c == 'Y');  // 其它任何键(包括裸回车)都算"否"

  auto result = pzt::core::create_tag(project_id, *name, cap, is_ordered);
  if (!result.ok()) return " 标签名 '" + *name + "' 已存在,未创建 ";
  return " 已创建标签 '" + *name + "' ";
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
    return " 没有可删除的标签 ";  // 不阻塞读键,跟"项目没有标签"同样的理由
  }

  std::string line = " 删除:";
  for (std::size_t i = 0; i < deletable.size(); ++i) {
    if (i > 0) line += "  ";
    line += std::to_string(i + 1) + ":" + deletable[i].name + "(" +
            std::to_string(deletable[i].tagged_count) + "张)";
  }
  line += "  Esc 取消";
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c < '1' || c > static_cast<char>('0' + deletable.size())) return "";  // 取消,静默
  const auto& chosen = deletable[static_cast<std::size_t>(c - '1')];

  std::string confirm = " 确定删除标签 '" + chosen.name + "'(" +
                         std::to_string(chosen.tagged_count) + " 张关联)?此操作不可撤销。"
                         "y 确认 / 其它键取消 ";
  char yn = prompt_and_read_key(confirm, banner_row, start_col, content_cols);
  if (yn != 'y' && yn != 'Y') return "";  // 取消,静默

  auto result = pzt::core::delete_tag(chosen.id);
  if (!result.ok()) return " 删除失败,请重试 ";  // 防御性,理论上不应该发生
  return " 已删除标签 '" + chosen.name + "' ";
}

}  // namespace

// `list_tags` 是按名字字母序排的(给 `pzt tag list` 这类展示用)——数字键
// 要按标签创建顺序(tag id 升序)固定,不然新建一个名字靠前的标签会让所有
// 已有标签的数字悄悄错位。不改 `list_tags` 本身,这里对结果客户端重排序。
// 只有 1-9 有数字键,超过的这次不处理。increment 6.4.5:系统标签("废片")
// 固定占硬编码的 `0`,不参与这个动态的 1-9 序列,先过滤掉。
std::vector<pzt::core::TagSummary> tags_for_menu(pzt::core::ProjectId project_id) {
  auto tags = pzt::core::list_tags(project_id);
  tags.erase(std::remove_if(tags.begin(), tags.end(), [](const auto& t) { return t.is_system; }),
             tags.end());
  std::sort(tags.begin(), tags.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  if (tags.size() > 9) tags.resize(9);
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
  return " 打标签失败,请重试 ";
}

// space 键的入口:在 banner 那一行显示可选标签、读一个键选标签或取消,
// cap 超限时转入 handle_cap_replace_submenu;`-`/`c`/`d` 分别转入摘除/新
// 建/删除标签定义。`0:废片` 是硬编码的固定选项,不占用 `tags_for_menu` 的
// 动态 1-9 序列——`废片` 从 pzt new 起就保证存在,不再有"这次按 space 完
// 全没有东西可选"的情况,不管动态列表是不是空都无条件阻塞读一个键。
std::string handle_space_key(pzt::core::ProjectId project_id, pzt::core::TagId reject_tag_id,
                              pzt::core::ImageId image_id, int banner_row, int start_col,
                              int content_cols) {
  auto tags = tags_for_menu(project_id);  // 只查一次,加/摘/删三个分支共用

  std::string line = " 0:废片";
  for (std::size_t i = 0; i < tags.size(); ++i) {
    line += "  " + std::to_string(i + 1) + ":" + tags[i].name;
    if (tags[i].cap) {
      line += "(" + std::to_string(tags[i].tagged_count) + "/" + std::to_string(*tags[i].cap) + ")";
    }
  }
  line += "  c:新建  d:删除  -:摘除  Esc 取消";
  char c = prompt_and_read_key(line, banner_row, start_col, content_cols);
  if (c == 'c') return handle_create_tag_flow(project_id, banner_row, start_col, content_cols);
  if (c == '-') {
    return handle_remove_tag_submenu(tags, reject_tag_id, image_id, banner_row, start_col,
                                      content_cols);
  }
  if (c == 'd') return handle_delete_tag_submenu(tags, banner_row, start_col, content_cols);
  if (c == '0') {
    return handle_add_tag_result(reject_tag_id, image_id, banner_row, start_col, content_cols);
  }
  if (c < '1' || c > static_cast<char>('0' + tags.size())) return "";  // 取消,静默

  const auto& chosen = tags[static_cast<std::size_t>(c - '1')];
  return handle_add_tag_result(chosen.id, image_id, banner_row, start_col, content_cols);
}

}  // namespace pzt::cli::menu
