#pragma once

#include <sqlite3.h>

#include <stdexcept>

// Schema for the global PZT metadata database. See docs/history/M0_Eng_Design.md
// "数据库 Schema 设计" for the authoritative design and rationale.
namespace pzt::core::db {

// 当前 schema 的版本号,记在库文件自己的 `PRAGMA user_version` 里(SQLite
// 留给应用用的一个 4 字节整数,默认 0)。T-7 之前 PZT 没有版本号,迁移全
// 靠"每次开库都把所有建表和加列检查跑一遍"的幂等性兜着。
//
// 版本 1 = T-7 落地时的 schema。因为默认值就是 0,"读到 0"同时覆盖了全新
// 空文件和任何 T-7 之前建的老库,两者走同一条全量初始化路径,跑完盖章。
//
// **改 schema 必须同时 bump 这个常量**:已盖章的库会跳过全部建表与加列
// 检查(见 initialize_schema),所以此后新加一条 ensure_column 不会再像从
// 前那样自动出现在存量库上,忘了 bump 就等于那一列在存量安装上永不出现。
// 每次 bump 配一个 migrate_vN_to_vN+1 步骤。详见 docs/RELEASE.md 的"数据
// 兼容性"一节。
inline constexpr int kSchemaVersion = 1;

// 库的 user_version 比当前二进制认识的还新 - 用户装过更新版的 pzt、又装
// 回了旧版。这种库的结构未知,旧代码往上写是在赌运气(新版新增的 NOT NULL
// 列会让旧版的 INSERT 直接失败),所以 initialize_schema 拒绝打开而不是
// 尽力而为。
//
// what() 是开发者向的英文诊断,跟 core 里其它 runtime_error 一个性质。
// core 不得携带面向用户的文案(见 docs/SPEC.md §4.1),两个版本号单独用
// 访问器暴露,让 cli/i18n 自己拼本地化句子,不用去解析 what()。
class SchemaTooNewError : public std::runtime_error {
 public:
  SchemaTooNewError(int found, int supported);

  int found_version() const { return found_; }
  int supported_version() const { return supported_; }

 private:
  int found_;
  int supported_;
};

// 把库带到 kSchemaVersion,并在这条连接上打开外键约束。每次开库都调用。
//
// 三条路径:版本已经是 kSchemaVersion 就只开外键直接返回(不跑任何 DDL);
// 版本更高抛 SchemaTooNewError;版本更低(今天只有 0)跑一次性迁移,在一个
// 事务里建表、补列、最后盖章。
void initialize_schema(sqlite3* conn);

}  // namespace pzt::core::db
