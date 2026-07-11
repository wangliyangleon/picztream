#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "core/ai/ai.h"

// 全局设置——静态、读一次的用户可配置行为(供应商、dedup 参数、批量操
// 作默认排除策略、界面偏好)，跟 db::default_db_path() 是同一类"core 读
// 自己的运行时配置"，不是业务数据。见 docs/Fix_It_Night_Review.md F-12
// 一节的完整设计背景。作用域先只做全局(不建项目级覆盖)；运行时
// `/setting` 开关不在这次范围，那是独立的未来任务(见同一节"未来任
// 务")，这里只有一个程序启动/命令触发时读一次、不能中途改的静态配置。
namespace pzt::core::settings {

struct Settings {
  ai::Provider ai_provider = ai::Provider::Gemini;
  int dedup_time_window_seconds = 10;
  int dedup_hash_threshold = 5;
  // F-26 的批量默认排除策略用的四个开关——true 表示"不排除"(把这类图
  // 片当成正常范围的一部分处理)，false(默认)是当前拍板的行为。
  bool eval_reject = false;
  bool dedup_reject = false;
  bool export_reject = false;
  bool export_dup = false;
  // nullopt = 配置文件里没写这个字段，cli::i18n::init_lang() 据此继续
  // 往下走系统 LANG 环境变量那一级——不能给一个"zh"之类的默认值，那样
  // 会没法区分"用户明确配置成中文"和"压根没配置"，导致系统 LANG 检测
  // 永远被这一级挡住。
  std::optional<std::string> lang;
  double ui_width_ratio = 0.7;
  std::size_t prefetch_window = 3;
};

// 照抄 db::default_db_path() 的 XDG 解析方式，落在同一个 ~/.config/pzt/
// 目录下的兄弟文件，不是新的配置根。
std::string default_config_path();

// 文件不存在 / 整个文件不是合法 JSON：返回 Settings{} 的默认值。文件
// 存在且是合法 JSON 对象时，逐个字段独立容错——某个字段缺失或者类型不
// 对，只有那一个字段回退到默认值，不连累其它已经解析成功的字段。
Settings load(const std::string& path = default_config_path());

}  // namespace pzt::core::settings
