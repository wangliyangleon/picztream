#pragma once

#include <optional>
#include <string>
#include <unordered_set>
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
  InvalidSyntax,        // 既不是 `*` / `.`，也不以 `#` 开头
  TagNotFound,          // 写法对，但项目里没有这个标签
  SystemTagNotAllowed,  // 范围是系统标签，而调用方声明了不接受（见 SystemTagPolicy）
  FilterFailed,         // 底层按标签过滤失败
  // 范围写的是 `.`，但调用方没给显式 id 集合（见 resolve 的 explicit_ids）。
  // 这一支不能并进 InvalidSyntax：`.` 是合法写法，只是这一侧没有集合可指，
  // 报语法错等于让 headless 说出"既不是 `*` 也不以 `#` 开头"这句假话，正是
  // T-10 与 U-3 那类"撒谎文案"（PRD #28 决策 D-6 第二条）。
  NoExplicitSet,
};

struct ScopeFailure {
  ScopeError error;
  // TagNotFound / SystemTagNotAllowed 时是解析出的 canonical 标签名，其它
  // 错误时为空。两个表现层的文案都要它（交互层的 err_console_tag_not_found
  // 与 headless 的 "tag not found: <name>"），而它们都不该为了拿这个名字
  // 再解析一遍 scope 字符串 —— 那正是本模块要消灭的东西。
  std::string tag_name;
};

struct Scope {
  std::vector<ImageId> image_ids;
  // 范围本身就是哪个标签，`*` 与 `.` 时为空（视图不是标签）。调用方用它
  // 判断 F-26 的对称例外（范围就是废片时不再排除废片），直接喂给
  // exclude_by_tags 的同名参数。
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

// 支持 `*`（整个项目）、`#标签名`、`#"带空格的标签名"`、`.`（调用方给定
// 的显式 id 集合）四种写法。标签名匹配大小写不敏感（沿用 find_tag_by_name
// 的 COLLATE NOCASE）。
//
// `.` 是 T-15（PRD #28 决策 D-4/D-6）加的第四支，core 侧的定义是"用
// explicit_ids 给的那一组，没给就是错"。参数的语义是**调用方给定的显式 id
// 集合**，不是"当前视图"这个 cli 概念 - core 不需要知道那组 id 从哪来，
// 所以这里没有分层泄漏；交互层传的是当前视图（`f` 筛选 ∩ `/filter` 之后
// 正在浏览的那批），headless 一侧没有可传的东西、于是拿到 NoExplicitSet。
//
// 之所以进 core 而不是在 cli 拦截：这份头注释是这套语法今天的**唯一权威
// 出处**，cli 分流会让它变成不完整的文档而读者无从知道要去拼第二处；且
// "这个 token 是不是合法作用域"归 core 判是 T-16 拍过的板。
//
// 这一支**不校验那组 id 属不属于 project_id**，另外三支则句句查库。这是
// "core 不需要知道那组 id 从哪来"的必然代价：能查的只有"这些 id 在不在这
// 个项目里"，而调用方本来就是从同一个项目的浏览列表里取的那批。写在这里
// 是让它成为一条明写的信任边界，而不是一个没人注意到的疏漏。
//
// nullptr 与"给了但是空的"是两件事：前者报 NoExplicitSet，后者是合法的空
// 作用域（`/filter` 一张都没筛出来是正常情况）。可选参数用指针而不是
// std::optional<std::vector> 正是为了分开这两者且不复制。集合原样透传、保
// 持顺序，`*` 与 `#标签` 两支完全不看它。
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
Result<Scope, ScopeFailure> resolve(db::Database& db, ProjectId project_id, const std::string& scope,
                                     SystemTagPolicy system_tag_policy = SystemTagPolicy::Allow,
                                     const std::vector<ImageId>* explicit_ids = nullptr);

// 排除规则本身：image_ids 里哪些图片带了 exclude_tag_names 里的标签、因而
// 该被剔除。
//
// scope_tag 非空且正好在排除列表里时，那个标签不参与排除（F-26 的对称例
// 外：用户显式把范围指成了废片，就是要处理废片）。直接传 resolve() 返回的
// Scope::scope_tag 即可。
//
// 标签在项目里不存在时按"不排除任何东西"处理，不是错误 —— 项目还没跑过
// `/dedup` 时"重复"标签根本不存在，那是正常情况。
//
// 暴露"排除集合"而不是只暴露下面那个 vector 版本，是因为调用方手里的容器
// 不一定是一串 ImageId：`core/export` 拿的是 ImageRef。需要统一的是**排哪
// 些**这条规则（哪些标签、对称例外、标签缺失怎么办），不是那一行
// remove_if。让 export 为了复用而先把 ImageRef 拆成 id 再拼回去，是拿真实
// 的复杂度换一个形式上的"零重复"。
std::unordered_set<ImageId> excluded_by_tags(db::Database& db, ProjectId project_id,
                                              const std::vector<ImageId>& image_ids,
                                              const std::vector<std::string>& exclude_tag_names,
                                              std::optional<TagId> scope_tag = std::nullopt);

// excluded_by_tags 之上的薄封装，给手里正好是一串 ImageId 的调用方
// （`core/tournament`、`cli` 的两条控制台路径）。保持输入顺序。
std::vector<ImageId> exclude_by_tags(db::Database& db, ProjectId project_id,
                                      const std::vector<ImageId>& image_ids,
                                      const std::vector<std::string>& exclude_tag_names,
                                      std::optional<TagId> scope_tag = std::nullopt);

}  // namespace pzt::core::scope
