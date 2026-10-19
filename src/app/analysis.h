#ifndef APP_ANALYSIS_H
#define APP_ANALYSIS_H

#include <chrono>
#include <cstddef>
#include <string>

#include "app/common.h"

namespace dsl {
class SourceMap;
}

namespace petri {
class PTPN;
}

namespace ptpn {
class Diagnostics;
}

namespace state_class {
struct MetricsReport;
}

namespace tdg {
class TDG;
}

namespace app {

// Analysis is forced when an SCG/metrics export is requested; otherwise
// `--no-analysis` is honored.
bool should_run_analysis(const PipelineOptions& opts);

// Post-load TDG path: lower to PTPN when needed, then analyze/export.
int run_tdg_analysis(const tdg::TDG& tdg, const PipelineOptions& opts,
                     const std::string& input_label,
                     std::chrono::high_resolution_clock::time_point pipeline_start,
                     size_t initial_memory, ptpn::Diagnostics* diagnostics,
                     const dsl::SourceMap* source_map = nullptr);

// Shared analysis/export stage for lowered TDGs and direct `.ptpn` nets.
int run_ptpn_postprocess(const petri::PTPN& ptpn, const std::string& input_label,
                         const PipelineOptions& opts,
                         std::chrono::high_resolution_clock::time_point pipeline_start,
                         size_t initial_memory, ptpn::Diagnostics* diagnostics = nullptr,
                         const dsl::SourceMap* source_map = nullptr);

// True when at least one export path was requested.
inline bool has_any_export(const ExportTargets& exports) {
  return !exports.tdg_dot.empty() || !exports.tdg_json.empty() || !exports.wcet_json.empty() ||
         !exports.ptpn_dot.empty() || !exports.scg_dot.empty() || !exports.metrics_json.empty();
}

// Writes the per-task WCET summary JSON (interval, segment count, and WCET
// provenance) for `graph` to `output_path`. The top-level `all_certified` flag
// is true only when every task's provenance kind is "certified". Returns false
// if the file cannot be opened, written, flushed or closed.
bool write_wcet_json(const tdg::TDG& graph, const std::string& output_path);

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

#endif  // APP_ANALYSIS_H
