#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "core/ai/ai.h"

// 全局设置——静态、读一次的用户可配置行为(供应商、dedup 参数、批量操
// 作默认排除策略、界面偏好)，跟 db::default_db_path() 是同一类"core 读
// 自己的运行时配置"，不是业务数据。见 docs/history/Fix_It_Night_Review.md F-12
// 一节的完整设计背景。作用域先只做全局(不建项目级覆盖)；运行时
// `/setting` 开关不在这次范围，那是独立的未来任务(见同一节"未来任
// 务")，这里只有一个程序启动/命令触发时读一次、不能中途改的静态配置。
namespace pzt::core::settings {

struct Settings {
  // 默认 Local(本地 Ollama)——本地优先、免云端配额;换云端在 config.json
  // 里把 ai_provider 设成 gemini/claude(并配好对应 API key)。
  ai::Provider ai_provider = ai::Provider::Local;
  // Provider::Local（Ollama）的连接信息——不是秘密，走 Settings 而不是
  // 环境变量，跟 curate_time_window_seconds 独立于 dedup_time_window_
  // seconds 同一个"可调行为参数放这里"的先例（见 docs/history/M4_Eng_Design.md
  // 第三节）。
  std::string ollama_base_url = "http://localhost:11434";
  std::string ollama_model = "gemma4:e2b";
  // 一次 AI 请求的墙钟上限(秒)，所有 provider、所有调用共用。默认 180。
  //
  // 票 06 真机验收踩出来的：这个数原先写死在 perform_curl_post 里是 60，
  // 而本地模型跑一次跨簇选择实测 45-80 秒(自报计算只有 2-6 秒，其余是加
  // 载与排队)，正好骑在那条线上，同一个项目有时退化有时不退化。云端
  // provider 一次就通，所以不是形状问题、是延迟问题。
  //
  // 调小的正当用途是"我不想为一次卡住的请求等三分钟"；调大的正当用途是
  // 更慢的本地模型或更大的预选集。非正值在 core 侧退回默认值(0 在
  // libcurl 里是"永不超时")，见 core/ai/ai.h::request_timeout_seconds。
  int ai_request_timeout_seconds = 180;
  int dedup_time_window_seconds = 10;
  int dedup_hash_threshold = 5;
  // B：curate 分簇用的阈值，独立于 dedup_*、不共用同一份配置（避免"调宽
  // 了 curate 顺带影响 dedup 标记"这种耦合）。候选集已经排除了"重复"标
  // 签图，用跟 dedup 相同的阈值重新分簇不会产生任何合并——默认值取
  // dedup 默认值(10s/5)的 2 倍，是留给真机效果调整的起点，不是精确调出
  // 来的数字。见 docs/history/M4_Eng_Design.md 第三节。
  int curate_time_window_seconds = 20;
  int curate_hash_threshold = 10;
  // 票 04 的倍率 M：curate 在选片之前先把候选集按时间多样性裁成
  // ceil(M · N) 张的预选集，跟上面两个 curate_* 同类，由 CLI 读出后显
  // 式传进 core(curate 本身不读 Settings)。默认 2。小于 1.5 的配置值在
  // core 侧按 1.5 生效 - M=1 时池子正好等于要选的数量，后续模型没有任何
  // 选择余地。见 docs/history/Intent_Curation_PRD.md 决策十一。
  double curate_preselect_multiplier = 2.0;
  // F-26 的批量默认排除策略用的四个开关——true 表示"不排除"(把这类图
  // 片当成正常范围的一部分处理)，false(默认)是当前拍板的行为。
  bool eval_reject = false;
  bool dedup_reject = false;
  bool export_reject = false;
  bool export_dup = false;
  // W2026-07-21：选片评估把这张图判为 unusable(有硬伤)时，自动打上
  // "废片"标签——默认 false(不自动打)。开着的话，后续默认排除废片的路
  // 径(eval_reject/dedup_reject/export_reject 均为 false 时)会连带把这
  // 些图挡在外面，不需要用户评估完之后手动再筛一遍逐张处理。只在
  // unusable 时打标签，反过来(重新评估后可用了)不会自动摘掉——摘除已
  // 有标签是更容易造成意外的操作，这次不做。
  bool auto_ai_reject = false;
  // T-10 (a)：终端不在 Kitty 协议白名单里时,`pzt open` 提示一句。判定是按
  // 环境变量猜的(见 cli/kitty/kitty.h::kitty_support_likely),必然有假阴
  // 性 - Ghostty 起的 tmux server 换终端 attach 之后信号就是 stale 的。这
  // 个开关是给那些用户的逃生口,设成 false 就不再提示。默认 true:静默失效
  // 比一行多余的提示更糟。
  bool warn_unsupported_terminal = true;
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
