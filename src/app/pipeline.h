#ifndef APP_PIPELINE_H
#define APP_PIPELINE_H

#include <cstddef>
#include <optional>
#include <string>

#include "analysis/allocation/allocation.h"
#include "model/types.h"

namespace tdg {
class TDG;
}

namespace ptpn {
class Diagnostics;
}

namespace app {

// CLI transport selection for the versioned diagnostics envelope.
// `output == "json"` writes the envelope to stdout; `file` writes it to PATH.
// Without either, no envelope is produced and CLI behavior is unchanged.
struct DiagnosticsOptions {
  std::string output;
  std::string file;

  [[nodiscard]] bool engaged() const {
    return !output.empty() || !file.empty();
  }
};

struct ExportTargets {
  std::string tdg_dot;
  // Materialized TDG JSON for the RTSL path (`ptpn rtsl --export-tdg`).
  std::string tdg_json;
  std::string wcet_json;
  std::string ptpn_dot;
  std::string scg_dot;
  std::string metrics_json;
};

struct PipelineOptions {
  size_t max_states = 10000;
  bool debug_mode = false;
  std::string canonicalization_mode = "equality";
  std::string reduction_mode = "none";
  bool extrapolation = false;
  // Keep the suspension clocks w_t in the state-class DBM. Off by default: the
  // exact projection preserves reachability and every metric while using a
  // smaller DBM.
  bool keep_suspension_clocks = false;
  bool skip_analysis = false;
  // WCRT time-horizon reduction: when set, the SCG is only expanded from state
  // classes whose accumulated *lower-bound* dwell is <= the horizon. States
  // beyond the horizon are still recorded (with their incoming edges) but are
  // not expanded. This is sound for WCRT bounds when the horizon is a valid
  // upper bound on the response time of interest; deadlock/SCC results are not
  // certified once pruning occurs. Disabled by default.
  std::optional<int> time_horizon;
  std::optional<SchedulePolicy> policy_override;
  DiagnosticsOptions diagnostics;
  ExportTargets exports;
};

struct CheckCommandOptions {
  std::string input_file;
  DiagnosticsOptions diagnostics;
};

struct AllocationCommandOptions {
  std::string input_file;
  std::string output_file;
  size_t max_candidates = 64;
  size_t candidate_max_states = 200000;
  allocation::Objective objective = allocation::Objective::MAX_WCRT;
  bool require_certified_wcet = false;
  bool debug_mode = false;
  std::optional<SchedulePolicy> policy_override;
  DiagnosticsOptions diagnostics;
};

size_t get_memory_usage();

// Writes the per-task WCET summary JSON (interval, segment count, and WCET
// provenance) for `graph` to `output_path`. The top-level `all_certified` flag
// is true only when every task's provenance kind is "certified". Returns false
// if the file cannot be opened.
bool write_wcet_json(const tdg::TDG& graph, const std::string& output_path);

// Prints diagnostics in the pre-envelope human-readable format. Shared by the
// `check` and `rtsl` commands so engaged/non-engaged output stays consistent.
void print_legacy_diagnostics(const ptpn::Diagnostics& diagnostics);

int run_tdg_pipeline(const std::string& input_file, PipelineOptions opts, size_t initial_memory,
                     ptpn::Diagnostics* diagnostics = nullptr);
// RTSL front-end: lex -> parse -> validate -> lower, then the same analysis
// pipeline as `run_tdg_pipeline`.
int run_rtsl_pipeline(const std::string& input_file, PipelineOptions opts, size_t initial_memory,
                      ptpn::Diagnostics* diagnostics = nullptr);
int run_ptpn_pipeline(const std::string& input_file, const PipelineOptions& opts,
                      size_t initial_memory, ptpn::Diagnostics* diagnostics = nullptr);
int run_allocation(const AllocationCommandOptions& opts, ptpn::Diagnostics* diagnostics = nullptr);

}  // namespace app

#endif  // APP_PIPELINE_H
