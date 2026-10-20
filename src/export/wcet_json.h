#ifndef EXPORT_WCET_JSON_H
#define EXPORT_WCET_JSON_H

#include <string>

#include "model/tdg.h"

namespace exporting {

// Writes the per-task WCET summary JSON (interval, segment count, and WCET
// provenance) for `graph` to `output_path`. The top-level `all_certified` flag
// is true only when every task's provenance kind is "certified". Returns false
// if the file cannot be opened, written, flushed or closed.
bool write_wcet_json(const tdg::TDG& graph, const std::string& output_path);

}  // namespace exporting

#endif  // EXPORT_WCET_JSON_H
