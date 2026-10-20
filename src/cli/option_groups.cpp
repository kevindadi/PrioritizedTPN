#include "cli/option_groups.h"

#include <string>

namespace cli {

void add_diagnostics_options(CLI::App* cmd, app::DiagnosticsOptions& diagnostics) {
  cmd->add_option("--diagnostics", diagnostics.output,
                  "Emit the versioned diagnostics JSON envelope on stdout (json)")
      ->check(CLI::IsMember({"json"}));
  cmd->add_option("--diagnostics-file", diagnostics.file,
                  "Write the versioned diagnostics JSON envelope to PATH");
}

SchedulePolicy parse_policy_option(const std::string& policy) {
  const SchedulePolicy parsed = parse_schedule_policy(policy);
  if (parsed == SchedulePolicy::UNKNOWN) {
    throw CLI::ValidationError(policy,
                               "Expected fixed, fixed_prior_with_restart, fixed_prior_with_resume, "
                               "fifo, rm, dm, edf, llf, pip, pcp, or srp");
  }
  return parsed;
}

void add_export_target_options(CLI::App* cmd, app::ExportTargets& exports,
                               bool tdg_export_is_json) {
  if (tdg_export_is_json) {
    cmd->add_option("--export-tdg", exports.tdg_json,
                    "Write the lowered TDG JSON (round-trippable through ptpn tdg) to PATH");
  } else {
    cmd->add_option("--export-tdg", exports.tdg_dot, "Write TDG Graphviz DOT to PATH");
  }
  cmd->add_option("--export-wcet", exports.wcet_json, "Write per-task WCET summary JSON to PATH");
  cmd->add_option("--export-ptpn", exports.ptpn_dot, "Write PTPN structure Graphviz DOT to PATH");
  cmd->add_option("--export-scg", exports.scg_dot,
                  "Write state-class reachability graph DOT to PATH");
  cmd->add_option("--export-metrics", exports.metrics_json,
                  "Write performance metrics JSON to PATH");
}

void add_common_pipeline_options(CLI::App* cmd, app::PipelineOptions& opts,
                                 bool tdg_export_is_json) {
  cmd->add_option("-m,--max-states", opts.max_states,
                  "Maximum number of states in reachability graph (default: 10000)");
  cmd->add_flag("--no-analysis", opts.skip_analysis, "Skip state-class reachability analysis");
  cmd->add_flag("--debug", opts.debug_mode, "Enable debug logging");
  cmd->add_option("--canonicalization", opts.canonicalization_mode,
                  "Canonicalization mode: equality, max-lower, or intersection "
                  "(default: equality)")
      ->check(CLI::IsMember({"equality", "max-lower", "intersection"}));
  cmd->add_option("--reduction", opts.reduction_mode,
                  "State-class reduction: none, upper-bound, cone, or symmetry. "
                  "upper-bound merges a candidate class into an existing class "
                  "with the same marking and active set when its zone is included "
                  "in the existing zone (WCRT is a sound upper bound and wcrt_lb "
                  "is unavailable); cone analyzes each task on its "
                  "dependency/contention cone and merges per-task metrics (needs "
                  "TDG lowering metadata; whole-net deadlock detection "
                  "unavailable); symmetry quotients verified permutations of "
                  "isomorphic tasks on distinct real cores (opt-in; pooled metrics "
                  "are conservative) (default: none)")
      ->check(CLI::IsMember({"none", "upper-bound", "cone", "symmetry"}));
  cmd->add_flag("--extrapolation", opts.extrapolation,
                "Enable k-extrapolation of clock zones: merges behaviorally "
                "equivalent state classes, preserving the reachable marking "
                "set (default: off)");
  cmd->add_flag("--keep-suspension-clocks", opts.keep_suspension_clocks,
                "Keep suspension clocks w_t in the state-class zone (default: "
                "project them out; exact for reachability and metrics)");
  cmd->add_option("--horizon", opts.time_horizon,
                  "Bound SCG exploration by a WCRT time horizon N: state classes "
                  "whose accumulated lower-bound dwell exceeds N are recorded but "
                  "not expanded (default: off). Sound for WCRT when N is an upper "
                  "bound on the response time of interest; deadlock/SCC results "
                  "are not certified when pruning occurs")
      ->check(CLI::NonNegativeNumber);
  cmd->add_option("--tie", opts.tie_mode,
                  "Equal-priority tie handling for fixed-priority policies. "
                  "fifo (default): a running transition keeps its core and an "
                  "equal-priority peer cannot preempt it. arbitrary: enumerate "
                  "every equal-priority choice, including equal-priority "
                  "preemption (worst case over tie-breaks)")
      ->check(CLI::IsMember({"fifo", "arbitrary"}));
  cmd->add_flag("--no-exact-wcrt", opts.no_exact_wcrt,
                "Disable the exact completion observation (release clocks): WCRT "
                "values fall back to sound dwell-DP bounds, which is faster on "
                "models with many tasks but makes deadline verdicts conservative "
                "(default: observation on)");
  cmd->add_option("--timeout", opts.timeout_seconds,
                  "Stop the state-class exploration after N seconds of wall clock "
                  "and report partial results (E-PERF-003, exit 5)")
      ->check(CLI::PositiveNumber);
  cmd->add_option("--max-memory", opts.max_memory_mb,
                  "Stop the state-class exploration once the process resident set "
                  "exceeds N MB and report partial results (E-PERF-004, exit 5)")
      ->check(CLI::PositiveNumber);
  add_diagnostics_options(cmd, opts.diagnostics);
  add_export_target_options(cmd, opts.exports, tdg_export_is_json);
}

}  // namespace cli
