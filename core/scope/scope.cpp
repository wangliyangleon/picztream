#include "core/scope/scope.h"

#include <cctype>
#include <unordered_set>

#include "core/browse/browse.h"

namespace pzt::core::scope {

namespace {

bool equals_ascii_case_insensitive(const std::string& a, const char* b) {
  std::size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return i == a.size() && b[i] == '\0';
}

// 用户打的标签名 -> canonical 存储名。只有系统标签有别名，普通标签原样返
// 回（它们照样享受 find_tag_by_name 自己的大小写不敏感匹配，只是不做语言
// 别名）。见头文件 D-3 那段。
//
// 别名的拼写来自 tagging::kRejectTagAlias/kDuplicateTagAlias，不在这里再写
// 一遍字面量：T-25 之后 `pzt images --json` 的 system_tags 字段用的是同一
// 组常量，两处必须逐字节一致，否则 headless 认得的词和输出的词会对不上。
std::string canonical_tag_name(const std::string& written) {
  if (written == tagging::kRejectTagName ||
      equals_ascii_case_insensitive(written, tagging::kRejectTagAlias)) {
    return tagging::kRejectTagName;
  }
  if (written == tagging::kDuplicateTagName ||
      equals_ascii_case_insensitive(written, tagging::kDuplicateTagAlias)) {
    return tagging::kDuplicateTagName;
  }
  return written;
}

bool is_system_tag_name(const std::string& canonical) {
  return tagging::system_tag_alias(canonical).has_value();
}

}  // namespace

Result<Scope, ScopeFailure> resolve(db::Database& db, ProjectId project_id, const std::string& scope,
                                     SystemTagPolicy system_tag_policy) {
  using R = Result<Scope, ScopeFailure>;

  if (scope == "*") {
    Scope result;
    for (const auto& ref : browse::list_images(db, project_id)) result.image_ids.push_back(ref.id);
    return R::Ok(std::move(result));
  }
  if (scope.empty() || scope[0] != '#') return R::Err(ScopeFailure{ScopeError::InvalidSyntax, ""});

  std::string tag_name = scope.substr(1);
  if (tag_name.size() >= 2 && tag_name.front() == '"' && tag_name.back() == '"') {
    tag_name = tag_name.substr(1, tag_name.size() - 2);
  }
  tag_name = canonical_tag_name(tag_name);

  // 查库之前先判策略，理由见头文件。
  if (system_tag_policy == SystemTagPolicy::Reject && is_system_tag_name(tag_name)) {
    return R::Err(ScopeFailure{ScopeError::SystemTagNotAllowed, tag_name});
  }

  auto tag_id = tagging::find_tag_by_name(db, project_id, tag_name);
  if (!tag_id) return R::Err(ScopeFailure{ScopeError::TagNotFound, tag_name});

  auto filtered = browse::filter_by_tag(db, *tag_id);
  if (!filtered.ok()) return R::Err(ScopeFailure{ScopeError::FilterFailed, tag_name});

  Scope result;
  result.scope_tag = *tag_id;
  for (const auto& ref : filtered.value()) result.image_ids.push_back(ref.id);
  return R::Ok(std::move(result));
}

std::unordered_set<ImageId> excluded_by_tags(db::Database& db, ProjectId project_id,
                                              const std::vector<ImageId>& image_ids,
                                              const std::vector<std::string>& exclude_tag_names,
                                              std::optional<TagId> scope_tag) {
  std::unordered_set<ImageId> excluded;
  for (const auto& tag_name : exclude_tag_names) {
    auto tag_id = tagging::find_tag_by_name(db, project_id, tag_name);
    if (!tag_id) continue;                            // 标签不存在 = 没有可排除的东西
    if (scope_tag && *scope_tag == *tag_id) continue;  // F-26 对称例外
    auto tagged = tagging::images_with_tag(db, image_ids, *tag_id);
    excluded.insert(tagged.begin(), tagged.end());
  }
  return excluded;
}

std::vector<ImageId> exclude_by_tags(db::Database& db, ProjectId project_id,
                                      const std::vector<ImageId>& image_ids,
                                      const std::vector<std::string>& exclude_tag_names,
                                      std::optional<TagId> scope_tag) {
  auto excluded = excluded_by_tags(db, project_id, image_ids, exclude_tag_names, scope_tag);
  if (excluded.empty()) return image_ids;

  std::vector<ImageId> kept;
  kept.reserve(image_ids.size());
  for (auto id : image_ids) {
    if (!excluded.count(id)) kept.push_back(id);
  }
  return kept;
}

}  // namespace pzt::core::scope
