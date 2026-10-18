#ifndef APP_PIPELINE_H
#define APP_PIPELINE_H

#include <cstddef>
#include <optional>
#include <string>

#include "model/types.h"

namespace app {

struct ExportTargets {
  std::string tdg_dot;
  std::string wcet_json;
  std::string ptpn_dot;
  std::string scg_dot;
  std::string metrics_json;
  std::string romeo_file;
  std::string ppn_file;
  std::string tina_file;
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
  std::string romeo_format = "scheduling-net";
  bool romeo_explicit_core_places = true;
  ExportTargets exports;
};

struct ExportCommandOptions {
  std::string input_file;
  std::string output_file;
  std::string input_format = "tdg";
  std::string romeo_format = "scheduling-net";
  bool romeo_explicit_core_places = true;
  std::optional<SchedulePolicy> policy_override;
  bool debug_mode = false;
};

size_t get_memory_usage();

int run_tdg_pipeline(const std::string& input_file, PipelineOptions opts, size_t initial_memory);
int run_ptpn_pipeline(const std::string& input_file, const PipelineOptions& opts,
                      size_t initial_memory);
int run_export_romeo(const ExportCommandOptions& opts);
int run_export_ptopner(const ExportCommandOptions& opts);

}  // namespace app

#endif  // APP_PIPELINE_H
