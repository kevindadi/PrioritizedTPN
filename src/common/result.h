#ifndef COMMON_RESULT_H
#define COMMON_RESULT_H

#include <optional>
#include <string>
#include <utility>

namespace ptpn {

// Minimal expected-like result: either a value or an error message. C++17 has
// no std::expected, and the codebase only needs success/failure plus a reason.
template <typename T>
class Result {
 public:
  static Result success(T value) {
    return Result(std::move(value), {});
  }

  static Result failure(std::string error) {
    return Result(std::nullopt, std::move(error));
  }

  [[nodiscard]] bool ok() const {
    return value_.has_value();
  }

  explicit operator bool() const {
    return ok();
  }

  [[nodiscard]] const T& value() const {
    return *value_;
  }

  [[nodiscard]] T& value() {
    return *value_;
  }

  [[nodiscard]] T&& take() {
    return std::move(*value_);
  }

  [[nodiscard]] const std::string& error() const {
    return error_;
  }

 private:
  Result(std::optional<T> value, std::string error)
      : value_(std::move(value)), error_(std::move(error)) {}

  std::optional<T> value_;
  std::string error_;
};

}  // namespace ptpn

#endif  // COMMON_RESULT_H
