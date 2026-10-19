#ifndef DSL_LOWER_H
#define DSL_LOWER_H

#include <optional>

#include "dsl/ast.h"
#include "dsl/source_map.h"
#include "dsl/validate.h"
#include "model/tdg.h"

namespace ptpn {
class Diagnostics;
}

namespace dsl {

// Lowers a validated RTSL program to the in-memory TDG. The result is
// deterministic: tasks and implicit fork/join nodes are inserted in canonical
// name order and edges in endpoint order, so a TDG JSON export round-trips to
// the same analysis regardless of source order.
//
// Validation owns user-facing rule checking; lowering re-checks the two
// graph-level rules that the JSON parser enforces but RTSL validation does not
// duplicate (core membership in allowed_cores, E-VAL-011, and segment count
// 2*lock_count+1, E-VAL-015) and reports them with SourceMap locations.
// `mode` mirrors ValidationOptions::Mode: measured tasks without intervals are
// only rejected outside Check mode (E-VAL-213).
std::optional<tdg::TDG> lower(const ast::Program& program, SourceMap& source_map,
                              ptpn::Diagnostics& diagnostics,
                              ValidationOptions::Mode mode = ValidationOptions::Mode::Rtsl);

}  // namespace dsl

#endif  // DSL_LOWER_H
