#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
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
      const decode::DecodedImage&, const std::string&, Provider)>;

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
  bool request(project::ImageId image_id, Provider provider, const std::string& extra_guidance);

  // 有没有请求正在排队或者处理中——跟 request() 的去重判断是两回事，这个
  // 是给"要不要显示一个全局的处理中提示"这类场景用的。
  bool has_pending() const;

  // 轮询用：每完成一个请求（无论成功失败）内部计数器就 +1。调用方保存
  // 上一次看到的值传进来比较，跟当前值不一样就说明有新结果落地了，函数
  // 返回 true 并把调用方持有的值更新到最新——调用方据此决定要不要重绘，
  // 不是每次 poll 都重绘。
  bool consume_new_result(std::uint64_t& last_seen_generation) const;

  // `/tasks` 用：排队中有几个、有没有正在处理中的一个。不展示具体是哪
  // 几张图片，只要数量和状态，见 docs/M3_PRD.md"批量评估与任务状态"一
  // 节。
  struct QueueStatus {
    std::size_t queued;
    bool processing;
  };
  // processing 靠 in_flight_.size() > queue_.size() 推出来，不需要单独
  // 一个"当前正在处理哪一个"的状态——worker_loop 是单线程的，任意时刻最
  // 多只有一个请求处于"已经从 queue_ 弹出、还没处理完"的状态，
  // in_flight_ 比 queue_ 多出来的那一个就是它。
  QueueStatus queue_status() const;

 private:
  struct PendingRequest {
    project::ImageId image_id;
    Provider provider;
    std::string extra_guidance;
  };

  void worker_loop(std::stop_token stop);
  void process_request(const PendingRequest& req);

  std::string db_path_;
  EvaluationFn evaluation_fn_;

  mutable std::mutex mu_;
  std::condition_variable_any cv_;
  std::vector<PendingRequest> queue_;
  std::unordered_set<project::ImageId> in_flight_;
  std::uint64_t generation_ = 0;

  // 声明在最后，保证析构时先于其它成员被销毁——它的析构自动
  // request_stop()+join()，worker 线程在其它成员真正被销毁之前已经彻底
  // 退出，不会访问悬空引用(照抄 PrefetchCache)。
  std::jthread worker_;
};

}  // namespace pzt::core::ai
