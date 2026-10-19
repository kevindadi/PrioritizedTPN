#ifndef APP_ANALYSIS_DIAGNOSTICS_H
#define APP_ANALYSIS_DIAGNOSTICS_H

#include <cstddef>

namespace ptpn {
class Diagnostics;
}

namespace state_class {
struct MetricsReport;
}

namespace app {

// Translates the metrics layer's signals into the diagnostics protocol:
// - a decisive counterexample found in the explored prefix (deadlock, local
//   deadlock, or wcrt_lb > deadline) is an analysis result (exit 6);
// - truncation without a counterexample is a performance warning (exit 5);
// - an upper-bound `wcrt` exceeding the deadline is never a counterexample.
//
// Also fills `result.analysis` when a collector is provided.
void record_analysis_diagnostics(ptpn::Diagnostics* diagnostics,
                                 const state_class::MetricsReport& report,
                                 bool max_states_truncated, bool horizon_truncated,
                                 size_t max_states);

}  // namespace app

#endif  // APP_ANALYSIS_DIAGNOSTICS_H
