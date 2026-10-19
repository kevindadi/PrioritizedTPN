#ifndef COMMON_UNSUPPORTED_CLASS_H
#define COMMON_UNSUPPORTED_CLASS_H

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace ptpn {

// Thrown when a model or option combination is outside an explicitly
// documented supported class. Deriving from std::invalid_argument keeps
// existing callers compatible, but the *type* is what determines the
// unsupported diagnostic category: callers must not classify based on
// exception text. `subject` uses the same structured convention as
// diagnostics (`task:<id>`, `policy:<name>`, `node:<name>`, `edge:<a->b>`,
// `option:<name>`); it is optional because some guards only know the policy.
class UnsupportedClassError : public std::invalid_argument {
 public:
  explicit UnsupportedClassError(std::string message,
                                 std::optional<std::string> subject = std::nullopt)
      : std::invalid_argument(std::move(message)), subject_(std::move(subject)) {}

  [[nodiscard]] const std::optional<std::string>& subject() const {
    return subject_;
  }

 private:
  std::optional<std::string> subject_;
};

}  // namespace ptpn

#endif  // COMMON_UNSUPPORTED_CLASS_H
