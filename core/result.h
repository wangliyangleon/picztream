#pragma once

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

// Project-wide error-handling convention (see docs/M0_Eng_Design.md 待确认问题):
// expected business outcomes (name taken, cap exceeded, project not found,
// ...) go through Result<T, E>, not exceptions. Exceptions/assertions are
// reserved for genuine programming errors (precondition violations).
namespace pzt::core {

template <typename T, typename E>
class Result {
 public:
  static Result Ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
  static Result Err(E error) { return Result(std::in_place_index<1>, std::move(error)); }

  bool ok() const { return data_.index() == 0; }

  const T& value() const {
    assert(ok());
    return std::get<0>(data_);
  }
  T& value() {
    assert(ok());
    return std::get<0>(data_);
  }

  const E& error() const {
    assert(!ok());
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
class Result<void, E> {
 public:
  static Result Ok() { return Result(); }
  static Result Err(E error) { return Result(std::move(error)); }

  bool ok() const { return !error_.has_value(); }

  const E& error() const {
    assert(!ok());
    return *error_;
  }

 private:
  Result() = default;
  explicit Result(E error) : error_(std::move(error)) {}

  std::optional<E> error_;
};

}  // namespace pzt::core
