#ifndef COMMON_DIAGNOSTICS_H
#define COMMON_DIAGNOSTICS_H

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ptpn {

// Machine-readable diagnostics for the LLM-facing CLI protocol. The same
// versioned envelope is written to stdout (`--diagnostics json`), to a file
// (`--diagnostics-file PATH`), or returned by future in-process callers.
// See docs/diagnostics.md for the code table and versioning policy.
constexpr const char* kDiagnosticsProtocol = "ptpn-diagnostics";
constexpr int kDiagnosticsVersion = 1;

// Optional source position. `line` and `column` are 1-based; 0 means unknown.
// A location with only `file` set is valid for parsers that do not track
// positions (for example the .ptpn parser).
struct SourceLocation {
  std::string file;
  int line = 0;
  int column = 0;
  int span = 0;
};

struct Diagnostic {
  std::string code;
  // Filled from the code prefix by Diagnostics::add() when left empty.
  std::string category;
  // "error" or "warning"; filled from the category when left empty.
  std::string severity;
  std::optional<SourceLocation> location;
  std::optional<std::string> subject;
  std::string message;
  std::optional<std::string> fix;
};

// Maps a stable code to its category: E-SYN -> syntax, E-VAL -> validation,
// E-UNS -> unsupported, E-PERF -> performance, R-ANA -> analysis,
// E-INT (and anything unrecognized) -> internal.
std::string category_for_code(const std::string& code);

// Default severity for a category: performance is a warning, everything else
// is an error.
std::string severity_for_category(const std::string& category);

// Collector for diagnostics with cascade suppression, a hard cap of
// kMaxDiagnostics entries (10), and the exit-code priority summary.
class Diagnostics {
 public:
  static constexpr std::size_t kMaxDiagnostics = 10;

  // Adds a diagnostic. Category/severity are derived from the code when not
  // set. Diagnostics whose subject has been marked tainted are dropped.
  // Entries beyond the cap are discarded and set truncated().
  void add(Diagnostic diagnostic);

  // Convenience overload.
  void add(const std::string& code, std::string message,
           std::optional<std::string> subject = std::nullopt,
           std::optional<SourceLocation> location = std::nullopt,
           std::optional<std::string> fix = std::nullopt);

  // Adds a warning-severity diagnostic (performance defaults to warning).
  void add_warning(const std::string& code, std::string message,
                   std::optional<std::string> subject = std::nullopt,
                   std::optional<SourceLocation> location = std::nullopt,
                   std::optional<std::string> fix = std::nullopt);

  // Cascade suppression hook (used by the RTSL validation stage in Phase 2):
  // diagnostics emitted against a tainted subject are dropped. Matching is
  // exact and keyed on the structured subject string.
  void mark_tainted(const std::string& subject);
  [[nodiscard]] bool is_tainted(const std::string& subject) const;

  // Copies entries (and the truncated flag) from another collector, applying
  // this collector's cap and taint set. Used to forward parser diagnostics.
  void append(const Diagnostics& other);

  // Payload included as `result` in the envelope. Empty/null results are
  // omitted from the serialized envelope.
  void set_result(nlohmann::json result);
  [[nodiscard]] const nlohmann::json& result() const;

  [[nodiscard]] const std::vector<Diagnostic>& entries() const {
    return entries_;
  }

  [[nodiscard]] bool empty() const {
    return entries_.empty();
  }

  [[nodiscard]] std::size_t size() const {
    return entries_.size();
  }

  [[nodiscard]] bool truncated() const {
    return truncated_;
  }

  void mark_truncated() {
    truncated_ = true;
  }

  // Removes every entry in `category`. Used when a stage is re-run (for
  // example validate() called twice on the same parser).
  void remove_category(const std::string& category);
  void clear();

  // Exit-code priority: internal -> 1 (dominant); otherwise
  // syntax (2) > validation (3) > unsupported (4) > analysis (6) >
  // performance (5) > 0. Only error-severity entries participate, except
  // that performance warnings map to 5 (partial results). A validation
  // warning alone therefore keeps the exit code at 0.
  [[nodiscard]] int exit_code() const;

  // True when the envelope reports success (exit_code() == 0).
  [[nodiscard]] bool ok() const {
    return exit_code() == 0;
  }

  // Builds the versioned envelope, including the stored result payload.
  [[nodiscard]] nlohmann::json to_json() const;

 private:
  std::vector<Diagnostic> entries_;
  std::set<std::string> tainted_;
  bool truncated_ = false;
  nlohmann::json result_;
};

}  // namespace ptpn

#endif  // COMMON_DIAGNOSTICS_H
