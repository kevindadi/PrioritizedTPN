#ifndef COMMON_DIAGNOSTIC_CODES_H
#define COMMON_DIAGNOSTIC_CODES_H

#include <string>
#include <vector>

namespace ptpn {

// One row of the diagnostic catalog documented in docs/diagnostics.md
// (section 5). The table is generated from the documentation by
// tools/gen_diagnostic_codes.py; test/test_explain.cpp fails when the two drift
// apart, so `ptpn explain` and the docs always agree.
struct DiagnosticCodeInfo {
  std::string code;      // e.g. "E-VAL-226"
  std::string category;  // syntax, validation, unsupported, analysis, performance, internal
  std::string severity;  // error, warning, hint
  std::string stage;     // which stage or command emits it
  std::string trigger;   // when it is emitted
  std::string subject;   // subject / location form
  std::string fix;       // suggested fix
  std::string group;     // documentation section it belongs to
};

// Every documented code, in documentation order.
const std::vector<DiagnosticCodeInfo>& diagnostic_code_catalog();

// The entry for `code`, or nullptr when the code is not documented.
const DiagnosticCodeInfo* find_diagnostic_code(const std::string& code);

}  // namespace ptpn

#endif  // COMMON_DIAGNOSTIC_CODES_H
