#ifndef APP_LOWERING_H
#define APP_LOWERING_H

namespace dsl {
class SourceMap;
}

namespace petri {
class PTPN;
}

namespace ptpn {
class Diagnostics;
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

}  // namespace app

#endif  // APP_LOWERING_H
