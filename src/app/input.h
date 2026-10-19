#ifndef APP_INPUT_H
#define APP_INPUT_H

#include <optional>
#include <string>

#include "dsl/ast.h"
#include "dsl/source_map.h"
#include "dsl/validate.h"
#include "model/tdg.h"
#include "model/types.h"

namespace parse {
class Parser;
}

namespace ptpn {
class Diagnostics;
}

namespace app {

// Reads the RTSL source without touching diagnostics. Callers decide how a
// missing file is reported: `ptpn check` keeps its legacy plain "ERROR:"
// rendering while the engaged/envelope paths use add_rtsl_open_error().
std::optional<std::string> read_rtsl_source(const std::string& input_file);

// Records the shared E-SYN-002 diagnostic for an unreadable RTSL file.
void add_rtsl_open_error(const std::string& input_file, ptpn::Diagnostics& diagnostics);

// Parsed and validated RTSL program plus the source map that anchors every
// structured subject to a byte-accurate span. `valid` is diagnostics.ok()
// after lex/parse/validate; a document is still returned when validation
// failed so callers can report task/edge counts.
struct RtslDocument {
  dsl::ast::Program program;
  dsl::SourceMap source_map;
  bool valid = false;
};

// Lex, panic-mode parse and validate. This stage never lowers; `ptpn check`
// relies on that boundary (lowering-only rules such as E-VAL-011/E-VAL-015
// stay unreported).
RtslDocument parse_and_validate_rtsl(std::string source, const std::string& input_file,
                                     dsl::ValidationOptions::Mode mode,
                                     ptpn::Diagnostics& diagnostics);

// Lowers a validated document to the in-memory TDG. Returns nullopt when the
// document is not valid or lowering reports errors.
std::optional<tdg::TDG> lower_rtsl_document(RtslDocument& document,
                                            dsl::ValidationOptions::Mode mode,
                                            ptpn::Diagnostics& diagnostics);

// Full RTSL input path (read, lex, parse, validate, lower). The returned source
// map must outlive the analysis because it anchors later lowering diagnostics
// (E-UNS-001 subjects such as `task:<id>`, `node:<name>`, `edge:<a->b>`).
struct RtslLoadResult {
  tdg::TDG tdg;
  dsl::SourceMap source_map;
};

std::optional<RtslLoadResult> load_tdg_from_rtsl(const std::string& input_file,
                                                 dsl::ValidationOptions::Mode mode,
                                                 ptpn::Diagnostics& diagnostics);

// Parses and validates TDG JSON, then fills `tdg` from the parsed graph.
// Returns 0 on success and 1 on failure; parser diagnostics are forwarded to
// `diagnostics` once after parse and once after validation, matching the
// legacy output ordering.
int load_tdg_from_json(const std::string& input_file, tdg::TDG& tdg, parse::Parser& parser,
                       ptpn::Diagnostics* diagnostics = nullptr);

// Applies `--policy` to the loaded TDG (no-op when unset).
void apply_policy_override(tdg::TDG& tdg, const std::optional<SchedulePolicy>& policy_override);
}  // namespace app

#endif  // APP_INPUT_H
