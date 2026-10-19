#ifndef APP_TDG_INPUT_H
#define APP_TDG_INPUT_H

#include <optional>
#include <string>

#include "model/types.h"

namespace parse {
class Parser;
}

namespace ptpn {
class Diagnostics;
}

namespace tdg {
class TDG;
}

namespace app {

// Parses and validates TDG JSON, then fills `tdg` from the parsed graph.
// Returns 0 on success and 1 on failure; parser diagnostics are forwarded to
// `diagnostics` once after parse and once after validation, matching the
// legacy output ordering.
int load_tdg_from_json(const std::string& input_file, tdg::TDG& tdg, parse::Parser& parser,
                       ptpn::Diagnostics* diagnostics = nullptr);

// Applies `--policy` to the loaded TDG (no-op when unset).
void apply_policy_override(tdg::TDG& tdg, const std::optional<SchedulePolicy>& policy_override);

}  // namespace app

#endif  // APP_TDG_INPUT_H
