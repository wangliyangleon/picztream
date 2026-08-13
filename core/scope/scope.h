#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/result.h"
#include "core/tagging/tagging.h"

// "一批图片的作用域 + 排除规则"的**唯一**实现（提案 T-16 / PRD #23）。
//
// 在这个模块存在之前，同一条规则散在六处：scope 解析在 cli 的交互路径与
// headless 路径各一份（后者的注释明确承认是有意重写），排除规则在
// core/tournament、core/dedup、core/curate、core/export、cli/browse、
// agent/stages/curate.py 各一份。分叉是实际发生过的，不是理论风险：
//
//   - headless 的解析不认系统标签英文别名，`pzt dedup --scope '#Reject'`
//     报 tag_not_found 而 `/dedup #Reject` 可用；
//   - core 侧无条件排废片(4cc6549)静默压死了 cli 层受 settings.dedup_reject
//     管的那份条件排除，`dedup_reject=true` 完全无效、`/dedup #废片` 变成
//     静默 no-op，两条都在 2026-07-11 随 F-26 真机验证通过过，坏了 22 天。
//
// 所以这里统一的是**机制**，不是策略：各命令排哪些标签、受哪个开关控制，
// 仍然由调用方决定（传不传这个标签名进来）。这个模块不认识 Settings。
namespace pzt::core::scope {

using project::ImageId;
using project::ProjectId;
using tagging::TagId;

// 结构化错误，不是人读文案 —— 这个模块有两个表现层完全不同的消费者：
// 交互层要 i18n 文案（还要跟着 config.json 的 lang 走），headless 要
// error_code/error_msg。把文案留在 core 会同时违反 SPEC §3.1 "core 不承载
// 任何面向用户的文案"和 §4.1 的分层契约。映射留在各自层不是重复实现，是
// 正确的分层。
enum class ScopeError {
  InvalidSyntax,        // 既不是 `*` 也不以 `#` 开头
  TagNotFound,          // 写法对，但项目里没有这个标签
  SystemTagNotAllowed,  // 范围是系统标签，而调用方声明了不接受（见 SystemTagPolicy）
  FilterFailed,         // 底层按标签过滤失败
};

struct Scope {
  std::vector<ImageId> image_ids;
  // 范围本身就是哪个标签，`*` 时为空。调用方用它判断 F-26 的对称例外
  // （范围就是废片时不再排除废片），直接喂给 exclude_by_tags 的同名参数。
  std::optional<TagId> scope_tag;
};

// 范围本身是系统标签（废片/重复）时怎么办。
//
// Allow 是默认，保证既有调用点行为不变：`/ai_eval #废片` 与 export 的目标
// 标签是废片/重复时，F-26 的对称例外让它们正常工作，今天有测试覆盖。
//
// Reject 供 dedup 用（PRD #23 决策 D-2）：`/dedup #废片` 今天是静默 no-op
// —— 范围被正确解析出来，进 core 后全被排除，命令报"0 组"，用户无从分辨
// 这是"真没有重复"还是"范围被清空了"。三条命令在这一点上不同构是有意的，
// 只有 dedup 存在"废片当上 keeper 把好邻居打成重复"这个失效模式。
enum class SystemTagPolicy { Allow, Reject };

// 支持 `*`（整个项目）、`#标签名`、`#"带空格的标签名"` 三种写法。标签名匹
// 配大小写不敏感（沿用 find_tag_by_name 的 COLLATE NOCASE）。
//
// 系统标签额外认稳定的 ASCII 别名 `#Reject` / `#Duplicate`（任意大小写）。
// 这是 PRD #23 决策 D-3：别名是**标识符**，不是显示文案。core 本来就把
// "废片"硬编码成系统标签的 canonical 存储名（tagging.h kRejectTagName），
// 再给它一个稳定的 ASCII 别名属于同一类事情；cli/i18n 负责的是**显示标
// 签**（会随界面语言变），而这里解析的是"用户打的这个词指哪个标签"（不随
// 界面语言变，本函数不读任何语言状态）。被否掉的替代方案是让 cli 注入别名
// 表 —— 那样 headless 仍然认不出 #Reject，除非它也注入同一张表，等于表还
// 是两份，分叉没消掉。
//
// SystemTagPolicy::Reject 的判定在**查库之前**、按 canonical 名字做：范围
// 写的是系统标签但项目里还没建过这个标签时，报 SystemTagNotAllowed 比报
// TagNotFound 更贴近用户真正做错的事。
Result<Scope, ScopeError> resolve(db::Database& db, ProjectId project_id, const std::string& scope,
                                   SystemTagPolicy system_tag_policy = SystemTagPolicy::Allow);

// 从 image_ids 里剔除带 exclude_tag_names 里任一标签的图片，保持输入顺序。
//
// scope_tag 非空且正好在排除列表里时，那个标签不参与排除（F-26 的对称例
// 外：用户显式把范围指成了废片，就是要处理废片）。直接传 resolve() 返回的
// Scope::scope_tag 即可。
//
// 标签在项目里不存在时按"不排除任何东西"处理，不是错误 —— 项目还没跑过
// `/dedup` 时"重复"标签根本不存在，那是正常情况。
std::vector<ImageId> exclude_by_tags(db::Database& db, ProjectId project_id,
                                      const std::vector<ImageId>& image_ids,
                                      const std::vector<std::string>& exclude_tag_names,
                                      std::optional<TagId> scope_tag = std::nullopt);

}  // namespace pzt::core::scope
