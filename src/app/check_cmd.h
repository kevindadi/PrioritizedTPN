#ifndef APP_CHECK_CMD_H
#define APP_CHECK_CMD_H

#include <string>

#include "app/pipeline.h"

namespace ptpn {
class Diagnostics;
}

namespace app {

// Runs the RTSL front-end only: lex -> parse -> validate. No lowering or
// analysis is performed (that arrives in Phase 2b with `ptpn rtsl`).
//
// When `diagnostics` is null (no envelope engaged) failures are printed to
// stderr in the legacy format and the function returns 0/1. When a collector
// is provided, the collector carries the versioned outcome (syntax 2,
// validation 3, unsupported 4) and a successful check stores
// `result.pending_measurements` for `measured by llm` tasks that still need
// intervals.
int run_rtsl_check(const CheckCommandOptions& opts, ptpn::Diagnostics* diagnostics);

}  // namespace app

#endif  // APP_CHECK_CMD_H
