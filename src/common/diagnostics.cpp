#include "common/diagnostics.h"

#include <algorithm>
#include <utility>

namespace ptpn {

namespace {

bool has_prefix(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

int category_rank(const std::string& category) {
  if (category == "syntax") {
    return 5;
  }
  if (category == "validation") {
    return 4;
  }
  if (category == "unsupported") {
    return 3;
  }
  if (category == "analysis") {
    return 2;
  }
  if (category == "performance") {
    return 1;
  }
  return 0;
}

int exit_code_for_rank(int rank) {
  switch (rank) {
    case 5:
      return 2;
    case 4:
      return 3;
    case 3:
      return 4;
    case 2:
      return 6;
    case 1:
      return 5;
    default:
      return 0;
  }
}

bool result_is_empty(const nlohmann::json& result) {
  return result.is_null() || (result.is_object() && result.empty());
}

}  // namespace

std::string category_for_code(const std::string& code) {
  if (has_prefix(code, "E-SYN-")) {
    return "syntax";
  }
  if (has_prefix(code, "E-VAL-")) {
    return "validation";
  }
  if (has_prefix(code, "E-UNS-")) {
    return "unsupported";
  }
  if (has_prefix(code, "E-PERF-")) {
    return "performance";
  }
  if (has_prefix(code, "R-ANA-")) {
    return "analysis";
  }
  if (has_prefix(code, "E-INT-")) {
    return "internal";
  }
  return "internal";
}

std::string severity_for_category(const std::string& category) {
  return category == "performance" ? "warning" : "error";
}

void Diagnostics::add(Diagnostic diagnostic) {
  if (diagnostic.code.empty()) {
    diagnostic.code = "E-INT-000";
  }
  if (diagnostic.category.empty()) {
    diagnostic.category = category_for_code(diagnostic.code);
  }
  if (diagnostic.severity.empty()) {
    diagnostic.severity = severity_for_category(diagnostic.category);
  }
  if (diagnostic.subject.has_value() && is_tainted(diagnostic.subject.value())) {
    return;
  }
  if (entries_.size() >= kMaxDiagnostics) {
    truncated_ = true;
    return;
  }
  entries_.push_back(std::move(diagnostic));
}

void Diagnostics::add(const std::string& code, std::string message,
                      std::optional<std::string> subject, std::optional<SourceLocation> location,
                      std::optional<std::string> fix) {
  Diagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.message = std::move(message);
  diagnostic.subject = std::move(subject);
  diagnostic.location = std::move(location);
  diagnostic.fix = std::move(fix);
  add(std::move(diagnostic));
}

void Diagnostics::add_warning(const std::string& code, std::string message,
                              std::optional<std::string> subject,
                              std::optional<SourceLocation> location,
                              std::optional<std::string> fix) {
  Diagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.message = std::move(message);
  diagnostic.subject = std::move(subject);
  diagnostic.location = std::move(location);
  diagnostic.fix = std::move(fix);
  diagnostic.severity = "warning";
  add(std::move(diagnostic));
}

void Diagnostics::mark_tainted(const std::string& subject) {
  tainted_.insert(subject);
}

bool Diagnostics::is_tainted(const std::string& subject) const {
  return tainted_.count(subject) != 0;
}

void Diagnostics::append(const Diagnostics& other) {
  truncated_ = truncated_ || other.truncated_;
  for (const Diagnostic& diagnostic : other.entries_) {
    add(diagnostic);
  }
}

void Diagnostics::set_result(nlohmann::json result) {
  result_ = std::move(result);
}

const nlohmann::json& Diagnostics::result() const {
  return result_;
}

void Diagnostics::remove_category(const std::string& category) {
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [&](const Diagnostic& diagnostic) { return diagnostic.category == category; }),
      entries_.end());
}

void Diagnostics::clear() {
  entries_.clear();
  truncated_ = false;
  result_ = nlohmann::json();
}

int Diagnostics::exit_code() const {
  int best_rank = 0;
  for (const Diagnostic& diagnostic : entries_) {
    if (diagnostic.category == "internal") {
      return 1;
    }
    // Performance diagnostics signal partial results even though their
    // default severity is "warning".
    if (diagnostic.category == "performance") {
      best_rank = std::max(best_rank, category_rank(diagnostic.category));
      continue;
    }
    if (diagnostic.severity != "error") {
      continue;
    }
    best_rank = std::max(best_rank, category_rank(diagnostic.category));
  }
  return exit_code_for_rank(best_rank);
}

nlohmann::json Diagnostics::to_json() const {
  nlohmann::json envelope = nlohmann::json::object();
  envelope["protocol"] = kDiagnosticsProtocol;
  envelope["version"] = kDiagnosticsVersion;
  envelope["ok"] = ok();
  envelope["exit_code"] = exit_code();
  envelope["diagnostics"] = nlohmann::json::array();
  for (const Diagnostic& diagnostic : entries_) {
    nlohmann::json entry = nlohmann::json::object();
    entry["code"] = diagnostic.code;
    entry["category"] = diagnostic.category;
    entry["severity"] = diagnostic.severity;
    if (diagnostic.location.has_value()) {
      const SourceLocation& location = diagnostic.location.value();
      if (!location.file.empty() || location.line > 0 || location.column > 0 || location.span > 0) {
        nlohmann::json serialized_location = nlohmann::json::object();
        if (!location.file.empty()) {
          serialized_location["file"] = location.file;
        }
        if (location.line > 0) {
          serialized_location["line"] = location.line;
        }
        if (location.column > 0) {
          serialized_location["column"] = location.column;
        }
        if (location.span > 0) {
          serialized_location["span"] = location.span;
        }
        entry["location"] = std::move(serialized_location);
      }
    }
    if (diagnostic.subject.has_value() && !diagnostic.subject->empty()) {
      entry["subject"] = diagnostic.subject.value();
    }
    entry["message"] = diagnostic.message;
    if (diagnostic.fix.has_value() && !diagnostic.fix->empty()) {
      entry["fix"] = diagnostic.fix.value();
    }
    envelope["diagnostics"].push_back(std::move(entry));
  }
  envelope["truncated_diagnostics"] = truncated_;
  if (!result_is_empty(result_)) {
    envelope["result"] = result_;
  }
  return envelope;
}

}  // namespace ptpn
