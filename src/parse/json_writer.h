#ifndef PARSE_JSON_WRITER_H
#define PARSE_JSON_WRITER_H

#include <nlohmann/json.hpp>
#include <string>

#include "model/tdg.h"

namespace parse {

// Materializes a TDG as the canonical JSON document understood by
// `ptpn tdg` (docs/json_format.md). Nodes are emitted in canonical order
// (tasks sorted by id, then implicit fork/join nodes sorted by name), so the
// output is deterministic and can be fed back through the JSON parser without
// changing analysis results.
nlohmann::json tdg_to_json(const tdg::TDG& tdg);

// Writes tdg_to_json(tdg) as pretty-printed JSON. Returns false when the output
// path cannot be opened.
bool write_tdg_json(const tdg::TDG& tdg, const std::string& output_path);

}  // namespace parse

#endif  // PARSE_JSON_WRITER_H
