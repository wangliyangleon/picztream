#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/browse/browse.h"
#include "core/decode/decode.h"
#include "core/result.h"

// 预取/缓存环形缓冲区。见 docs/M0_Eng_Design.md increment 6.3。围绕当前浏览
// 位置前后各 window 张，用 std::jthread 驱动的后台线程负责"读文件字节 +
// JPEG 解码到像素"这一整条链路(呼应渲染延迟验证结论第 2 条:解码必须发生
// 在异步预取阶段，不能算进按键触发的同步延迟预算)。这里只负责调度(什么
// 时候解码、结果放哪)，不重复实现解码本身，复用 core/decode。
//
// 这是"当前浏览到哪张图"状态之外，increment 6 引入的另一块单次进程运行内
// 的内存状态：cli 的全键盘循环(6.4)每次导航后调用 set_current()，再用
// get() 取当前图片的已解码像素。
//
// 延迟敏感路径:每次解码完成、每次 get() 命中/未命中都往 stderr 打一行延
// 迟日志(AGENTS.md 要求延迟敏感路径配套延迟日志，不能只靠单元测试)。
namespace pzt::core::browse {

// path -> 解码结果，签名与 decode::decode_jpeg_file 一致。测试用假实现替换
// 真实解码，避免单元测试依赖真实 JPEG 文件与解码耗时。
using DecodeFn = std::function<Result<decode::DecodedImage, decode::DecodeError>(const std::string&)>;

enum class FetchError {
  NotInWindow,   // 请求的图片不在当前预取窗口内，调用方不应该等它
  DecodeFailed,  // 在窗口内，但后台解码失败(文件缺失/损坏)
};

class PrefetchCache {
 public:
  // root_path 用来把 ImageRef::file_path(相对路径)拼成可以喂给 decode_fn
  // 的绝对路径。window 是当前索引前后各预取多少张，具体默认值留给调用方
  // (cli)按真实素材测出来，这里不预设。
  PrefetchCache(std::string root_path, std::size_t window,
                DecodeFn decode_fn = decode::decode_jpeg_file);
  ~PrefetchCache();

  PrefetchCache(const PrefetchCache&) = delete;
  PrefetchCache& operator=(const PrefetchCache&) = delete;

  // 浏览导航之后调用:重新计算 [current 前 window 张, current 后 window 张]
  // 这个窗口(在 images 上按循环语义取，跟 next_image/prev_image 的折返规则
  // 一致)，异步调度窗口内缺失的解码任务，驱逐窗口外的缓存条目。不阻塞——
  // 真正的解码在后台线程完成。current_id 为 nullopt 或不在 images 里时清空
  // 窗口，不调度任何解码。
  void set_current(const std::vector<ImageRef>& images, std::optional<ImageId> current_id);

  // 取一张已解码图片。不在当前窗口内直接返回 NotInWindow，不做同步解码
  // (不能把解码算进按键触发的同步延迟预算)。在窗口内但后台还没解码完时阻
  // 塞等待(典型场景:刚导航到窗口边缘、预取还没跟上)，这个等待耗时就是
  // "按键到画面"链路里预取没命中时的真实延迟，会记进延迟日志。
  // F-14：返回 shared_ptr(指向缓存里那份不可变像素)而不是按值拷贝整张图——
  // 24MP 一张就是 96MB，原来在持 mu_ 期间整块拷贝、拷贝时长内 worker 全阻
  // 塞。调用方只读使用,共享同一份即可。
  Result<std::shared_ptr<const decode::DecodedImage>, FetchError> get(ImageId id);

 private:
  enum class State { Pending, Ready, Failed };

  struct Entry {
    State state = State::Pending;
    std::shared_ptr<const decode::DecodedImage> image;  // F-14：共享不可变像素,避免持锁整块拷贝
  };

  void worker_loop(std::stop_token stop);

  std::string root_path_;
  std::size_t window_;
  DecodeFn decode_fn_;

  std::mutex mu_;
  std::condition_variable_any cv_;
  std::unordered_map<ImageId, Entry> cache_;
  std::vector<ImageId> pending_queue_;  // 按调度优先级排列，front 先解码
  std::unordered_map<ImageId, std::string> paths_;  // 队列/worker 用，id -> 绝对路径

  std::jthread worker_;
};

}  // namespace pzt::core::browse
