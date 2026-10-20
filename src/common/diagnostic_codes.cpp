#include "common/diagnostic_codes.h"

#include <algorithm>
#include <iterator>

namespace ptpn {

namespace {

// Generated from docs/diagnostics.md by tools/gen_diagnostic_codes.py.
const DiagnosticCodeInfo kDiagnosticCodes[] = {
#include "common/diagnostic_codes.inc"
};

}  // namespace

const std::vector<DiagnosticCodeInfo>& diagnostic_code_catalog() {
  static const std::vector<DiagnosticCodeInfo> catalog(std::begin(kDiagnosticCodes),
                                                       std::end(kDiagnosticCodes));
  return catalog;
}

const DiagnosticCodeInfo* find_diagnostic_code(const std::string& code) {
  const std::vector<DiagnosticCodeInfo>& catalog = diagnostic_code_catalog();
  const auto it = std::find_if(catalog.begin(), catalog.end(),
                               [&](const DiagnosticCodeInfo& info) { return info.code == code; });
  return it == catalog.end() ? nullptr : &*it;
}

}  // namespace ptpn
