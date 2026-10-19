#ifndef APP_LOWERING_H
#define APP_LOWERING_H

#include <optional>
#include <string>

namespace dsl {
class SourceMap;
}

namespace petri {
class PTPN;
}

namespace ptpn {
class Diagnostics;
struct SourceLocation;
}

namespace tdg {
class TDG;
}

namespace app {

// Lowers a TDG to a PTPN and translates lowering failures into diagnostics:
// supported-class guards become E-UNS-001 (with a structured subject and an
// optional RTSL source location), anything else E-INT-003.
int build_ptpn_from_tdg(const tdg::TDG& tdg, petri::PTPN& ptpn,
                        ptpn::Diagnostics* diagnostics = nullptr,
                        const dsl::SourceMap* source_map = nullptr);

// Resolves a lowering/analysis subject to its RTSL SourceMap span. Implicit
// nodes are bound under `fork:<name>` / `join:<name>` and the policy under
// `policy`, while lowering reports `node:<name>` / `policy:<name>`. Returns
// nullopt when the subject has no reliable source position.
std::optional<ptpn::SourceLocation> source_map_location(const dsl::SourceMap& source_map,
                                                        const std::string& subject);

}  // namespace app

#endif  // APP_LOWERING_H
