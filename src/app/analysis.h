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

}  // namespace app

#endif  // APP_ANALYSIS_H
