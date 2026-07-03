#include <doctest.h>

#include <string>

#include "core/result.h"

using pzt::core::Result;

enum class TestError { Bad };

TEST_CASE("Result<T,E> carries a success value") {
  auto r = Result<int, TestError>::Ok(42);
  REQUIRE(r.ok());
  CHECK(r.value() == 42);
}

TEST_CASE("Result<T,E> carries an error value") {
  auto r = Result<int, TestError>::Err(TestError::Bad);
  REQUIRE(!r.ok());
  CHECK(r.error() == TestError::Bad);
}

TEST_CASE("Result<void,E> Ok/Err") {
  auto ok = Result<void, TestError>::Ok();
  CHECK(ok.ok());

  auto err = Result<void, TestError>::Err(TestError::Bad);
  REQUIRE(!err.ok());
  CHECK(err.error() == TestError::Bad);
}

TEST_CASE("Result<T,E> works with non-trivial T") {
  auto r = Result<std::string, TestError>::Ok("hello");
  REQUIRE(r.ok());
  CHECK(r.value() == "hello");
}
