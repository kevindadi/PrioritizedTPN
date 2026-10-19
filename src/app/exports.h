#ifndef APP_EXPORTS_H
#define APP_EXPORTS_H

#include <string>

#include "app/common.h"

namespace tdg {
class TDG;
}

namespace app {

// True when at least one export path was requested.
inline bool has_any_export(const ExportTargets& exports) {
  return !exports.tdg_dot.empty() || !exports.tdg_json.empty() || !exports.wcet_json.empty() ||
         !exports.ptpn_dot.empty() || !exports.scg_dot.empty() || !exports.metrics_json.empty();
}

// Writes the per-task WCET summary JSON (interval, segment count, and WCET
// provenance) for `graph` to `output_path`. The top-level `all_certified` flag
// is true only when every task's provenance kind is "certified". Returns false
// if the file cannot be opened.
bool write_wcet_json(const tdg::TDG& graph, const std::string& output_path);

}  // namespace app

#endif  // APP_EXPORTS_H
