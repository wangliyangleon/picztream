#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>
#include <variant>

// Project-wide error-handling convention (see docs/M0_Eng_Design.md 待确认问题):
// expected business outcomes (name taken, cap exceeded, project not found,
// ...) go through Result<T, E>, not exceptions. Exceptions/assertions are
// reserved for genuine programming errors (precondition violations).
namespace pzt::core {

namespace detail {

// F-19：value()/error() 读错变体是编程错误(调用方没先查 ok()),不是运行
// 时该处理的业务结果——那种情况走 ok() 分支。以前用 assert() 兜底,但
// RelWithDebInfo(build_release/,用户实际测试用的构建)默认带 -DNDEBUG,
// assert 在这个配置下被完全编译掉,读错变体会变成未定义行为而不是可预
// 测的崩溃。这里不依赖 assert,两种构建配置下都 fail-fast。
[[noreturn]] inline void fail_wrong_variant(const char* what) {
  std::fprintf(stderr,
               "pzt: Result<> %s - this is a programming error (caller didn't check ok() "
               "first), not a runtime condition\n",
               what);
  std::abort();
}

}  // namespace detail

// F-19：整个类标 [[nodiscard]] 而不是逐个函数标——这样任何返回
// Result<T,E> 的函数(core/api.h 里几十个)的调用方如果没有检查返回值就
// 直接丢弃,都会触发编译警告,不需要在每一个消费点/每一个 core 函数签
// 名上重复这个属性。真实触发过的问题(F-18)：dedup 忽略 add_tag 的
// Result,导致 tagged_count 统计跟实际打标签结果对不上。
template <typename T, typename E>
class [[nodiscard]] Result {
 public:
  static Result Ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
  static Result Err(E error) { return Result(std::in_place_index<1>, std::move(error)); }

  bool ok() const { return data_.index() == 0; }

  const T& value() const {
    if (!ok()) detail::fail_wrong_variant("value() called on an Err");
    return std::get<0>(data_);
  }
  T& value() {
    if (!ok()) detail::fail_wrong_variant("value() called on an Err");
    return std::get<0>(data_);
  }

  const E& error() const {
    if (ok()) detail::fail_wrong_variant("error() called on an Ok");
    return std::get<1>(data_);
  }

 private:
  template <typename... Args>
  explicit Result(Args&&... args) : data_(std::forward<Args>(args)...) {}

  std::variant<T, E> data_;
};

// Specialization for operations with no success payload (archive_project,
// delete_project, remove_tag, ...).
template <typename E>
class [[nodiscard]] Result<void, E> {
 public:
  static Result Ok() { return Result(); }
  static Result Err(E error) { return Result(std::move(error)); }

  bool ok() const { return !error_.has_value(); }

  const E& error() const {
    if (ok()) detail::fail_wrong_variant("error() called on an Ok");
    return *error_;
  }

 private:
  Result() = default;
  explicit Result(E error) : error_(std::move(error)) {}

  std::optional<E> error_;
};

}  // namespace pzt::core
