#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

#include <spdlog/spdlog.h>

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
  DiagnosticsOptions diagnostics;
  ExportTargets exports;
};

size_t get_memory_usage();

// Prints diagnostics in the pre-envelope human-readable format. Shared by the
// `check` and pipeline commands so engaged/non-engaged output stays consistent.
void print_legacy_diagnostics(const ptpn::Diagnostics& diagnostics);


namespace detail {

using PipelineClock = std::chrono::high_resolution_clock;

inline long long elapsed_ms(const PipelineClock::time_point& start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(PipelineClock::now() - start)
      .count();
}

// The `[STATS]` text is parsed by example/run.py; keep the format stable.
inline void log_step_timing(const std::string& step, long long ms,
                            const std::string& detail = {}) {
  if (detail.empty()) {
    spdlog::info("[STATS] {}: {} ms", step, ms);
  } else {
    spdlog::info("[STATS] {}: {} ms ({})", step, ms, detail);
  }
}

// Temporarily raises the global log level (used to keep per-candidate lowering
// and analysis logging out of the default allocation output). Restores the
// previous level on scope exit, including through early returns.
class ScopedLogLevel {
 public:
  explicit ScopedLogLevel(spdlog::level::level_enum level)
      : previous_level_(spdlog::default_logger()->level()) {
    spdlog::set_level(level);
  }

  ~ScopedLogLevel() {
    spdlog::set_level(previous_level_);
  }

  ScopedLogLevel(const ScopedLogLevel&) = delete;
  ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;

 private:
  spdlog::level::level_enum previous_level_;
};

}  // namespace detail

}  // namespace app

#endif  // APP_COMMON_H
