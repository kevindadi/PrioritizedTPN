#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

#include "model/types.h"

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
  // Equal-priority tie handling for the scalar-priority rules: "fifo" (default,
  // no equal-priority preemption) or "arbitrary". Kept as the CLI string; the
  // analysis layer converts it with parse_tie_mode. See TieMode in
  // model/types.h.
  std::string tie_mode = "fifo";
  // Disable the exact completion observation (release clocks) and keep the
  // dwell-DP WCRT bounds. Faster on models with many tasks, but the values are
  // bounds and deadline verdicts become conservative.
  bool no_exact_wcrt = false;
  DiagnosticsOptions diagnostics;
  ExportTargets exports;
};

// Peak resident memory of this process in megabytes (all platforms).
size_t get_memory_usage_mb();

// Prints diagnostics in the pre-envelope human-readable format. Shared by the
// `check` and pipeline commands so engaged/non-engaged output stays consistent.
void print_legacy_diagnostics(const ptpn::Diagnostics& diagnostics);


namespace detail {

using PipelineClock = std::chrono::high_resolution_clock;

inline long long elapsed_ms(const PipelineClock::time_point& start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(PipelineClock::now() - start)
      .count();
}

}  // namespace detail

}  // namespace app

#endif  // APP_COMMON_H
