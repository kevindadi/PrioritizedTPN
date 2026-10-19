#ifndef DSL_VALIDATE_H
#define DSL_VALIDATE_H

#include <nlohmann/json.hpp>

#include "common/diagnostics.h"
#include "dsl/ast.h"
#include "dsl/source_map.h"

namespace dsl {

struct ValidationOptions {
  // `check` accepts models whose `measured by llm` intervals have not been
  // filled in yet. `rtsl`/`allocate` reject them with E-VAL-213.
  enum class Mode { Check, Rtsl, Allocate };

  Mode mode = Mode::Check;
};

// Validates the parsed program and reports every reachable semantic rule.
// Diagnostics whose subject was marked tainted by the parser are dropped by
// the collector. Also binds `lock:<name>` entries into the SourceMap.
void validate(const ast::Program& program, SourceMap& source_map, ptpn::Diagnostics& diagnostics,
              const ValidationOptions& options = {});

// Builds the `result.pending_measurements` payload for a successful check:
// one entry per `measured by llm` task whose intervals are absent. `inject` is
// the full `task:<id>:time` span used for slice replacement.
nlohmann::json collect_pending_measurements(const ast::Program& program,
                                            const SourceMap& source_map);

}  // namespace dsl

#endif  // DSL_VALIDATE_H
