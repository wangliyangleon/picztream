#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/ai/ai.h"
#include "core/ai/evaluation.h"
#include "core/db/database.h"
#include "core/project/project.h"

// 把 core::ai::request_evaluation 接到后台线程上：CLI 按 `:` 提交请求立
// 即返回，不阻塞任何按键；同一张图重复请求会被去重；结果（无论成功失
// 败）直接落库，不在内存里保留，调用方靠 consume_new_result 轮询"有没有
// 新结果落地"来决定要不要重绘。骨架照抄 core::browse::PrefetchCache（同
// 一个 jthread+mutex+condition_variable_any 的先例），取代了 M3 增量一最
// 初那版的 core::ai::ScoreWorker（已删除）。
namespace pzt::core::ai {

class EvaluationWorker {
 public:
  using EvaluationFn = std::function<Result<EvaluationResult, EvaluationError>(
      const decode::DecodedImage&, const std::string&, Provider, Language, const LocalModelConfig&)>;

  // db_path 默认真实的全局数据库路径；测试传一个临时路径，跟仓库里其它
  // 所有测试统一用 Database::open_at(fresh_db_path(...)) 的写法一致，不
  // 需要摆弄 XDG_CONFIG_HOME。evaluation_fn 默认真实调用 AI
  // (request_evaluation)；测试注入假函数，不连真实网络。
  explicit EvaluationWorker(std::string db_path = db::default_db_path(),
                             EvaluationFn evaluation_fn = request_evaluation);
  ~EvaluationWorker();

  EvaluationWorker(const EvaluationWorker&) = delete;
  EvaluationWorker& operator=(const EvaluationWorker&) = delete;

  // 提交一个评估请求，立即返回，不阻塞。同一张图已经有请求在排队/处理
  // 中时返回 false（去重——调用方据此提示"正在处理"，不是报错）。
  // auto_reject 是显式参数，不是从 Settings.auto_ai_reject 读——
  // process_request 本身不知道、也不该知道调用方是交互路径还是 agent，
  // 见 docs/history/M4_PRD.md P6"物理隔离"：agent 触发时可以比人工更激进（评估
  // 不达标直接打废片），交互路径的默认设置完全不受影响。调用方（
  // browse.cpp）自己决定传什么值——交互路径传
  // load_settings().auto_ai_reject，行为跟以前一样。
  // language：assessment 的输出语言(guidance 为空时用它)，cli 按当前界面
  // 语言映射后传进来，默认 Chinese。见 core/ai/evaluation.h 的 Language。
  bool request(project::ImageId image_id, Provider provider, const std::string& extra_guidance,
               bool auto_reject, Language language = Language::Chinese,
               const LocalModelConfig& local_config = LocalModelConfig{});

  // 有没有请求正在排队或者处理中——跟 request() 的去重判断是两回事，这个
  // 是给"要不要显示一个全局的处理中提示"这类场景用的。
  bool has_pending() const;

  // 轮询用：每完成一个请求（无论成功失败）内部计数器就 +1。调用方保存
  // 上一次看到的值传进来比较，跟当前值不一样就说明有新结果落地了，函数
  // 返回 true 并把调用方持有的值更新到最新——调用方据此决定要不要重绘，
  // 不是每次 poll 都重绘。
  bool consume_new_result(std::uint64_t& last_seen_generation) const;

  // `/tasks` 用：排队中有几个、有没有正在处理中的一个。不展示具体是哪
  // 几张图片，只要数量和状态，见 docs/history/M3_PRD.md"批量评估与任务状态"一
  // 节。
  // T-23：加上 failed - 本 worker 存续期间(即这一次 `pzt open`)累计失败
  // 了几张，从不清零。状态行错过了就没了，`/tasks` 是唯一能事后回看
  // "刚才那几条失败到底挂了多少张"的地方。跟 FailureReport 报的是同一
  // 个数字，只是那边顺带带上最近一次的图片与原因。
  struct QueueStatus {
    std::size_t queued;
    bool processing;
    std::size_t failed;
  };
  // processing 靠 in_flight_.size() > queue_.size() 推出来，不需要单独
  // 一个"当前正在处理哪一个"的状态——worker_loop 是单线程的，任意时刻最
  // 多只有一个请求处于"已经从 queue_ 弹出、还没处理完"的状态，
  // in_flight_ 比 queue_ 多出来的那一个就是它。
  QueueStatus queue_status() const;

  // F-03：评估请求失败(网络/key/解析，或者请求真正发出去之前就失败
  // - 图片/项目找不到、预览图解码失败)之前只打 stderr，不开 --debug
  // 时用户完全看不到，提交之后要么等到结果、要么永远等不到也不知道为
  // 什么。跟 generation_ 用途不同：generation_ 只回答"有没有新结果落
  // 地"(成功/失败都算)，这个回答"有没有新的失败要报、最近失败的是哪张
  // 图、什么原因、到这一刻一共挂了几张"。有新失败才返回值,取走即清空
  // (跟 consume_new_result 的"消费一次"精神一致)，调用方(browse.cpp 的
  // poll 逻辑)只在确认有新结果落地之后才取一次,不会重复弹同一条提示。
  //
  // T-23：原来这里只留最后一条(`last_failure_` 无条件覆盖)。批量评估
  // 全批失败时,后台线程处理请求的速度远快于 300ms 一次的 poll，前面那
  // 些失败在被取走之前就被后面的盖掉了 - 用户看到的是零星几条互不相干
  // 的提示,既不知道失败了多少张，也没有"该停下来检查环境"的信号(而这
  // 类失败恰恰高度相关:key 没配、Ollama 没起来，一挂就是整批)。
  //
  // total_failed 刻意是**累计值**而不是"自上次取走以来的增量"。增量看
  // 着更贴合"这次要报什么",但它解决不了原问题:失败一条一条落地时(网
  // 络超时那种,每条都要烧掉一次超时,落地间隔远大于 poll 周期)，每次取
  // 走都只增了 1,状态行就退化回"IMG_x 评估失败"刷屏、一个总数都看不
  // 到 - 正是 U-12 点名的场景。累计值在同一条路径上会一路涨到 37,那才
  // 是"值得停下来检查环境"的信号。失败具体是哪几张不值得逐条列出来
  // (状态行只有一行),最近那一条带着具体原因就够了。
  struct FailureReport {
    project::ImageId last_image_id;
    EvaluationError last_error;
    std::size_t total_failed;  // 本次 pzt open 累计失败张数,跟 QueueStatus::failed 同源
  };
  std::optional<FailureReport> take_failure_report();

 private:
  struct PendingRequest {
    project::ImageId image_id;
    Provider provider;
    std::string extra_guidance;
    bool auto_reject;
    Language language;
    LocalModelConfig local_config;
  };

  void worker_loop(std::stop_token stop);
  // 返回 nullopt 表示这次请求成功；否则携带失败原因，worker_loop 负责
  // 把它记进 pending_failure_(process_request 本身不碰 mu_ 保护的状态，
  // 维持它原来"纯粹是 db I/O + 网络调用"的定位）。
  std::optional<EvaluationError> process_request(const PendingRequest& req);
  // 真正干活的那一半。拆出来是为了让 process_request 只剩一层 catch-all：
  // 这条线程上会 throw 的地方远不止一处(Database::open_at、db::Stmt 的构
  // 造函数、project/tagging 里所有 DAO 风格的函数遇到 sqlite 失败都是
  // throw)，而 jthread 上任何未捕获的异常都是 std::terminate。
  std::optional<EvaluationError> process_request_impl(const PendingRequest& req);

  std::string db_path_;
  EvaluationFn evaluation_fn_;

  mutable std::mutex mu_;
  std::condition_variable_any cv_;
  std::vector<PendingRequest> queue_;
  std::unordered_set<project::ImageId> in_flight_;
  std::uint64_t generation_ = 0;
  // pending_failure_ 只回答"有没有新失败还没报给用户"(取走即清空)，失
  // 败张数本身一律从 failed_total_ 读,那个从不清零。理由见 FailureReport
  // 上的注释。
  std::optional<FailureReport> pending_failure_;
  std::size_t failed_total_ = 0;

  // 声明在最后，保证析构时先于其它成员被销毁——它的析构自动
  // request_stop()+join()，worker 线程在其它成员真正被销毁之前已经彻底
  // 退出，不会访问悬空引用(照抄 PrefetchCache)。
  std::jthread worker_;
};

}  // namespace pzt::core::ai
