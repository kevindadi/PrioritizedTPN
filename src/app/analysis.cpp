#include "app/analysis.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reductions/cone_projection.h"
#include "app/lowering.h"
#include "common/diagnostics.h"
#include "common/unsupported_class.h"
#include "dsl/source_map.h"
#include "export/export_dot.h"
#include "export/metrics_json.h"
#include "export/export_ptpn.h"
#include "export/scg_dot.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "model/types.h"

namespace app {

namespace {

struct ConeAnalysisResult {
  state_class::MetricsReport report;
  long long duration_ms = 0;
  size_t tasks = 0;
};

long long lcm_ll(long long a, long long b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  const long long divisor = std::gcd(a, b);
  return a / divisor * b;
}

state_class::CanonicalizationMode parse_canonicalization(const std::string& mode) {
  if (mode == "max-lower") {
    return state_class::CanonicalizationMode::MAX_LOWER_BOUND;
  }
  if (mode == "intersection") {
    return state_class::CanonicalizationMode::INTERSECTION;
  }
  return state_class::CanonicalizationMode::EQUALITY;
}

state_class::ReductionMode parse_reduction(const std::string& mode) {
  if (mode == "upper-bound") {
    return state_class::ReductionMode::UPPER_BOUND;
  }
  return state_class::ReductionMode::NONE;
}

// Per-task cone decomposition: for every task, project the net onto its cone,
// build the state-class graph of that sub-net and keep only the focus task's
// TaskMetrics. The merged report answers the same per-task questions as the
// full-graph analysis with a much smaller state space when tasks are loosely
// coupled.
//
// Whole-net deadlock discovery and graph-wide lock statistics cannot be
// reconstructed from independent cones. Cone mode leaves deadlock_states empty
// and derives schedulability from the per-task verdicts; use full-graph mode
// when whole-net deadlock detection is required.
bool analyze_task_cones(const petri::PTPN& ptpn, const PipelineOptions& opts,
                        state_class::CanonicalizationMode canonicalization,
                        ConeAnalysisResult& result) {
  if (ptpn.task_info().empty()) {
    return false;
  }

  const auto cone_start = detail::PipelineClock::now();
  state_class::MetricsReport merged;
  merged.exact = true;
  merged.bounded = true;
  merged.wcrt_lb_valid = true;
  merged.deadlock_analysis_certified = true;

  std::vector<std::string> task_names;
  task_names.reserve(ptpn.task_info().size());
  for (const auto& [name, info] : ptpn.task_info()) {
    (void)info;
    task_names.push_back(name);
  }
  std::sort(task_names.begin(), task_names.end());

  std::vector<state_class::TaskMetrics> task_metrics;
  task_metrics.reserve(task_names.size());

  for (const std::string& task_name : task_names) {
    petri::PTPN cone;
    try {
      cone = petri::project_cone(ptpn, task_name);
    } catch (const std::exception& error) {
      spdlog::warn("[CONE] Cannot project task '{}': {}", task_name, error.what());
      return false;
    }

    state_class::StateClassReachabilityGraph graph(cone);
    graph.set_canonicalization_mode(canonicalization);
    if (opts.extrapolation) {
      graph.set_extrapolation(true);
    }
    graph.build(opts.max_states);
    const auto& stats = graph.get_statistics();
    const bool cone_exact = canonicalization == state_class::CanonicalizationMode::EQUALITY &&
                            !opts.extrapolation && !stats.truncated;
    merged.exact = merged.exact && cone_exact;
    merged.truncated = merged.truncated || stats.truncated;
    merged.states += stats.total_states;
    merged.transitions += stats.total_transitions;

    state_class::MetricsAnalyzer analyzer(graph.get_graph(), cone, graph.get_initial_vertices(),
                                          cone_exact,
                                          /*wcrt_lb_valid=*/true);
    state_class::MetricsReport cone_report = analyzer.analyze();
    merged.bounded = merged.bounded && cone_report.bounded;
    merged.overflow_places.insert(merged.overflow_places.end(), cone_report.overflow_places.begin(),
                                  cone_report.overflow_places.end());
    merged.has_steady_cycle = merged.has_steady_cycle || cone_report.has_steady_cycle;
    merged.recurrent_scc_size = std::max(merged.recurrent_scc_size, cone_report.recurrent_scc_size);

    const auto metric_it = std::find_if(
        cone_report.tasks.begin(), cone_report.tasks.end(),
        [&](const state_class::TaskMetrics& metrics) { return metrics.name == task_name; });
    if (metric_it == cone_report.tasks.end()) {
      spdlog::warn("[CONE] Cone for task '{}' produced no task metrics", task_name);
      return false;
    }
    task_metrics.push_back(*metric_it);

    spdlog::debug("[CONE] {}: places={}, transitions={}, states={}, truncated={}", task_name,
                  cone.num_places(), cone.num_transitions(), stats.total_states,
                  stats.truncated ? "true" : "false");
  }

  merged.tasks = std::move(task_metrics);

  long long hyperperiod = 0;
  for (const auto& [name, info] : ptpn.task_info()) {
    (void)name;
    if (info.period > 0) {
      hyperperiod = hyperperiod == 0 ? info.period : lcm_ll(hyperperiod, info.period);
    }
  }
  merged.hyperperiod = hyperperiod;
  for (auto& metrics : merged.tasks) {
    metrics.jobs_per_hyperperiod = (metrics.period > 0 && hyperperiod > 0)
                                       ? static_cast<int>(hyperperiod / metrics.period)
                                       : 0;
  }

  std::set<int> cores;
  for (const auto& [name, info] : ptpn.task_info()) {
    (void)name;
    if (info.core >= 0) {
      cores.insert(info.core);
    }
  }
  for (int core : cores) {
    state_class::CoreMetrics metrics;
    metrics.core = core;
    for (const auto& [name, info] : ptpn.task_info()) {
      (void)name;
      if (info.core != core || info.period <= 0) {
        continue;
      }
      metrics.util_min += static_cast<double>(info.bcet) / info.period;
      metrics.util_max += static_cast<double>(info.wcet) / info.period;
    }
    merged.cores.push_back(metrics);
  }

  // Whole-net deadlock discovery is intentionally not approximated from the
  // cones; schedulability keeps the per-task part.
  merged.deadlock_states.clear();
  merged.schedulable = true;
  for (const auto& metrics : merged.tasks) {
    if ((metrics.has_deadline && metrics.deadline_missed) || metrics.local_deadlock) {
      merged.schedulable = false;
    }
  }

  result.report = std::move(merged);
  result.duration_ms = detail::elapsed_ms(cone_start);
  result.tasks = task_names.size();
  return true;
}

}  // namespace

bool should_run_analysis(const PipelineOptions& opts) {
  if (!opts.exports.scg_dot.empty() || !opts.exports.metrics_json.empty()) {
    return true;
  }
  return !opts.skip_analysis;
}

int run_ptpn_postprocess(const petri::PTPN& ptpn, const std::string& input_label,
                         const PipelineOptions& opts,
                         std::chrono::high_resolution_clock::time_point pipeline_start,
                         size_t initial_memory, ptpn::Diagnostics* diagnostics,
                         const dsl::SourceMap* source_map) try {
  if (opts.debug_mode) {
    std::cout << ptpn.to_string();
  }

  const auto export_model = petri::exporting::build_export_model(ptpn);

  if (!opts.exports.ptpn_dot.empty()) {
    if (petri::exporting::save_to_dot(export_model, opts.exports.ptpn_dot)) {
      spdlog::info("[OUTPUT] PTPN DOT exported to: {}", opts.exports.ptpn_dot);
    } else {
      spdlog::warn("[OUTPUT] Failed to export PTPN DOT");
      if (diagnostics != nullptr) {
        diagnostics->add("E-INT-002", "failed to write PTPN DOT export: " + opts.exports.ptpn_dot,
                         "output:" + opts.exports.ptpn_dot, std::nullopt,
                         "check that the output path is writable");
      }
      return 1;
    }
  }

  if (opts.reduction_mode == "cone" && should_run_analysis(opts)) {
    if (!opts.exports.scg_dot.empty()) {
      spdlog::warn(
          "[CONE] --export-scg requires a single state-class graph; falling back "
          "to full-graph analysis");
    } else {
      if (opts.time_horizon.has_value()) {
        spdlog::warn("[CONE] --horizon is ignored in cone mode");
      }
      const auto canonicalization = parse_canonicalization(opts.canonicalization_mode);
      ConeAnalysisResult cone_result;
      if (analyze_task_cones(ptpn, opts, canonicalization, cone_result)) {
        const state_class::MetricsReport& report = cone_result.report;
        record_analysis_diagnostics(diagnostics, report, report.truncated,
                                    /*horizon_truncated=*/false, opts.max_states);
        detail::log_step_timing(
            "Cone analysis", cone_result.duration_ms,
            "tasks=" + std::to_string(cone_result.tasks) + ", states=" +
                std::to_string(report.states) + ", edges=" + std::to_string(report.transitions));
        if (!opts.exports.metrics_json.empty()) {
          if (!state_class::save_metrics_json(report, opts.exports.metrics_json)) {
            spdlog::warn("[OUTPUT] Failed to export metrics");
            if (diagnostics != nullptr) {
              diagnostics->add("E-INT-002",
                               "failed to write metrics JSON: " + opts.exports.metrics_json,
                               "output:" + opts.exports.metrics_json, std::nullopt,
                               "check that the output path is writable");
            }
            return 1;
          }
          spdlog::info("[OUTPUT] Metrics exported to: {}", opts.exports.metrics_json);
          spdlog::info("[METRICS] schedulable={}, bounded={}, deadlocks={}",
                       report.schedulable ? "true" : "false", report.bounded ? "true" : "false",
                       report.deadlock_states.size());
        }
        const auto end_time = detail::PipelineClock::now();
        const auto total_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - pipeline_start);
        const size_t total_memory = get_memory_usage() - initial_memory;
        detail::log_step_timing(input_label + " pipeline (total)", total_duration.count(),
                                std::to_string(total_memory) + " KB");
        return 0;
      }
      spdlog::warn("[CONE] Cone analysis unavailable; falling back to full-graph analysis");
    }
  }

  if (should_run_analysis(opts)) {
    const auto canonicalization = parse_canonicalization(opts.canonicalization_mode);
    const auto reduction = parse_reduction(opts.reduction_mode);
    const bool upper_bound_merging = reduction == state_class::ReductionMode::UPPER_BOUND;
    state_class::StateClassReachabilityGraph reachability_graph(ptpn);
    reachability_graph.set_canonicalization_mode(canonicalization);
    reachability_graph.set_suspension_clock_projection(!opts.keep_suspension_clocks);
    reachability_graph.set_reduction_mode(reduction);
    spdlog::info("[SCG] Canonicalization mode: {}", opts.canonicalization_mode);
    if (upper_bound_merging) {
      spdlog::info(
          "[SCG] Reduction: upper-bound zone merging (sound over-approximation; "
          "WCRT is an upper bound, wcrt_lb unavailable)");
      if (canonicalization != state_class::CanonicalizationMode::EQUALITY) {
        spdlog::warn("[SCG] Reduction 'upper-bound' takes precedence over canonicalization '{}'",
                     opts.canonicalization_mode);
      }
    } else if (canonicalization != state_class::CanonicalizationMode::EQUALITY) {
      spdlog::warn(
          "[SCG] Canonicalization '{}' uses the zone-inclusion implementation; "
          "max-lower and intersection are currently aliases",
          opts.canonicalization_mode);
    }
    spdlog::info("[SCG] Priority semantics: enumerate all admissible active sets");
    spdlog::info("[SCG] Suspension clock projection: {}",
                 opts.keep_suspension_clocks ? "off (keeping w_t)" : "on (dropping w_t)");
    if (opts.reduction_mode == "symmetry") {
      reachability_graph.set_symmetry_reduction(true);
      if (reachability_graph.symmetry_applied()) {
        spdlog::info(
            "[SCG] Reduction: symmetry quotient (verified task permutations, "
            "group order {})",
            reachability_graph.symmetry_group_order());
      } else {
        spdlog::info(
            "[SCG] Reduction: symmetry requested, but no verified task "
            "permutation applies; graph is not reduced");
      }
    } else if (!upper_bound_merging) {
      spdlog::info("[SCG] Reduction: none");
    }
    if (opts.extrapolation) {
      reachability_graph.set_extrapolation(true);
      spdlog::info("[SCG] k-extrapolation enabled (k={})",
                   reachability_graph.extrapolation_bound());
    }
    if (opts.time_horizon.has_value()) {
      reachability_graph.set_time_horizon(opts.time_horizon.value());
      spdlog::info(
          "[SCG] WCRT time horizon enabled (H={}): classes whose accumulated "
          "lower-bound dwell exceeds H are recorded but not expanded",
          opts.time_horizon.value());
      if (canonicalization != state_class::CanonicalizationMode::EQUALITY) {
        spdlog::warn(
            "[SCG] Time horizon is combined with canonicalization '{}'; WCRT "
            "values are not certified even when the horizon itself is large "
            "enough (use --canonicalization equality)",
            opts.canonicalization_mode);
      }
    }
    const auto scg_start = detail::PipelineClock::now();
    const size_t state_count = reachability_graph.build(opts.max_states);
    const auto& reachability_stats = reachability_graph.get_statistics();
    detail::log_step_timing(
        "SCG build", detail::elapsed_ms(scg_start),
        "states=" + std::to_string(reachability_stats.total_states) +
            ", edges=" + std::to_string(reachability_stats.total_transitions) +
            ", dedup_hits=" + std::to_string(reachability_stats.dedup_hits) +
            ", zone_merges=" + std::to_string(reachability_stats.zone_merges) +
            ", merged_states=" + std::to_string(reachability_stats.merged_states) +
            (reachability_stats.horizon_truncated
                 ? ", horizon_pruned=" + std::to_string(reachability_stats.horizon_pruned_states)
                 : "") +
            (reachability_stats.truncated ? ", truncated" : ""));
    if (reachability_stats.horizon_truncated) {
      spdlog::warn(
          "[SCG] Time horizon pruned expansion of {} state classes; WCRT values "
          "are approximate unless H is a valid upper bound on the response time "
          "of interest, and deadlock/SCC results are NOT certified",
          reachability_stats.horizon_pruned_states);
    } else if (opts.time_horizon.has_value()) {
      spdlog::info("[SCG] No class exceeded the time horizon; graph is exhaustive");
    }
    if (reachability_stats.truncated) {
      spdlog::warn(
          "[SCG] Reachability graph truncated at {} states; use --max-states "
          "to raise the bound",
          opts.max_states);
    } else {
      spdlog::info("[SCG] Reachability graph built with {} states", state_count);
    }

    if (!opts.exports.scg_dot.empty()) {
      const auto scg_export_start = detail::PipelineClock::now();
      const scg_export::ScgFormatter formatter(ptpn, reachability_graph.get_graph());
      if (formatter.save_to_dot(opts.exports.scg_dot)) {
        detail::log_step_timing("SCG DOT export", detail::elapsed_ms(scg_export_start));
        spdlog::info("[OUTPUT] State class graph exported to: {}", opts.exports.scg_dot);
      } else {
        spdlog::warn("[OUTPUT] Failed to export state class graph");
        if (diagnostics != nullptr) {
          diagnostics->add("E-INT-002",
                           "failed to write state-class DOT export: " + opts.exports.scg_dot,
                           "output:" + opts.exports.scg_dot, std::nullopt,
                           "check that the output path is writable");
        }
        return 1;
      }
    }

    // Metrics feed both the optional JSON export and the diagnostics analysis
    // facts. They are computed when either consumer needs them.
    const bool need_metrics = !opts.exports.metrics_json.empty() || diagnostics != nullptr;
    if (need_metrics) {
      // Metrics are exact only for the complete, un-abstracted graph. Zone
      // extrapolation preserves reachable markings but relaxes timing bounds,
      // upper-bound zone merging over-approximates the zone space, truncation
      // omits part of the state space, and a horizon cut makes WCRT sound only
      // under the caller's H >= WCRT assumption. Symmetry quotients keep all
      // reachable classes, but graph edges carry representative task labels;
      // timing values are pooled within verified groups and therefore reported
      // as bounds rather than certified exact.
      const bool horizon_truncated =
          opts.time_horizon.has_value() && reachability_stats.horizon_truncated;
      const bool symmetry_used = reachability_graph.symmetry_applied();
      const bool exact = canonicalization == state_class::CanonicalizationMode::EQUALITY &&
                         !opts.extrapolation && !upper_bound_merging &&
                         !reachability_stats.truncated && !horizon_truncated && !symmetry_used;
      const bool export_metrics = !opts.exports.metrics_json.empty();
      if (export_metrics && !exact) {
        spdlog::warn(
            "[METRICS] Metrics are approximate (canonicalization='{}', "
            "extrapolation={}, reduction='{}', truncated={}, "
            "horizon_truncated={}, symmetry={}); timing values are bounds, not "
            "exact values",
            opts.canonicalization_mode, opts.extrapolation, opts.reduction_mode,
            reachability_stats.truncated ? "true" : "false", horizon_truncated ? "true" : "false",
            symmetry_used ? "true" : "false");
      }
      if (export_metrics && upper_bound_merging) {
        spdlog::warn(
            "[METRICS] wcrt_lb is not certified on the merged graph and is "
            "reported as unavailable (null)");
      }
      const auto metrics_start = detail::PipelineClock::now();
      state_class::MetricsAnalyzer analyzer(reachability_graph.get_graph(), ptpn,
                                            reachability_graph.get_initial_vertices(), exact,
                                            /*wcrt_lb_valid=*/!upper_bound_merging);
      state_class::HorizonInfo horizon_info;
      if (opts.time_horizon.has_value()) {
        horizon_info.enabled = true;
        horizon_info.bound = opts.time_horizon.value();
        horizon_info.truncated = reachability_stats.horizon_truncated;
        horizon_info.pruned_states = reachability_stats.horizon_pruned_states;
      }
      analyzer.set_horizon_info(horizon_info);
      analyzer.set_symmetry_task_groups(reachability_graph.symmetry_task_groups());
      const state_class::MetricsReport report = analyzer.analyze();
      record_analysis_diagnostics(diagnostics, report, reachability_stats.truncated,
                                  horizon_truncated, opts.max_states);
      if (export_metrics) {
        if (state_class::save_metrics_json(report, opts.exports.metrics_json)) {
          detail::log_step_timing("Metrics analysis", detail::elapsed_ms(metrics_start));
          spdlog::info("[OUTPUT] Metrics exported to: {}", opts.exports.metrics_json);
          if (report.deadlock_analysis_certified) {
            spdlog::info("[METRICS] schedulable={}, bounded={}, deadlocks={}",
                         report.schedulable ? "true" : "false", report.bounded ? "true" : "false",
                         report.deadlock_states.size());
          } else {
            spdlog::warn(
                "[METRICS] deadlock/schedulability/SCC results are not certified "
                "(time horizon truncated the state space); schedulability=unknown");
          }
        } else {
          spdlog::warn("[OUTPUT] Failed to export metrics");
          if (diagnostics != nullptr) {
            diagnostics->add("E-INT-002",
                             "failed to write metrics JSON: " + opts.exports.metrics_json,
                             "output:" + opts.exports.metrics_json, std::nullopt,
                             "check that the output path is writable");
          }
          return 1;
        }
      }
    }
  } else {
    spdlog::info("[SCG] Reachability analysis skipped");
  }

  const auto end_time = detail::PipelineClock::now();
  const auto total_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - pipeline_start);
  const size_t total_memory = get_memory_usage() - initial_memory;
  detail::log_step_timing(input_label + " pipeline (total)", total_duration.count(),
                          std::to_string(total_memory) + " KB");
  return 0;
} catch (const ::ptpn::UnsupportedClassError& error) {
  // The analysis layer rejects models/options that pass lowering but are
  // outside a rule's supported class (for example an EDF/FIFO/LLF overlapping
  // release, or a job-clock extrapolation/symmetry combination). This is a
  // model/option limit, not an internal failure.
  spdlog::error("[ANALYSIS] Unsupported model or option: {}", error.what());
  if (diagnostics != nullptr) {
    const std::string subject =
        error.subject().value_or("policy:" + schedule_policy_to_string(ptpn.schedule_policy()));
    std::optional<::ptpn::SourceLocation> location;
    if (source_map != nullptr) {
      location = source_map_location(*source_map, subject);
    }
    diagnostics->add("E-UNS-001", error.what(), subject, std::move(location),
                     "simplify the model or choose a supported policy/option");
  }
  return 1;
}

// Shared post-load TDG pipeline: PTPN lowering, state-class analysis, metrics
// and the total timing line. `tdg` and `rtsl` both go through this function so
// analysis and export semantics cannot diverge (C1).
int run_tdg_analysis(const tdg::TDG& tdg, const PipelineOptions& opts,
                     const std::string& input_label,
                     std::chrono::high_resolution_clock::time_point pipeline_start,
                     size_t initial_memory, ptpn::Diagnostics* diagnostics,
                     const dsl::SourceMap* source_map) {
  const bool needs_ptpn = should_run_analysis(opts) || !opts.exports.ptpn_dot.empty() ||
                          !opts.exports.scg_dot.empty() || !opts.exports.metrics_json.empty();

  if (!needs_ptpn) {
    if (!has_any_export(opts.exports)) {
      spdlog::warn("[MAIN] No exports requested and analysis disabled; nothing to do");
      return 0;
    }
    const auto end_time = detail::PipelineClock::now();
    detail::log_step_timing(input_label + " pipeline (total)", detail::elapsed_ms(pipeline_start),
                            std::to_string(get_memory_usage() - initial_memory) + " KB");
    return 0;
  }

  petri::PTPN ptpn;
  const auto lowering_start = detail::PipelineClock::now();
  if (build_ptpn_from_tdg(tdg, ptpn, diagnostics, source_map) != 0) {
    return 1;
  }
  detail::log_step_timing("TDG2PN lowering", detail::elapsed_ms(lowering_start));

  return run_ptpn_postprocess(ptpn, input_label, opts, pipeline_start, initial_memory, diagnostics,
                              source_map);
}


void record_analysis_diagnostics(ptpn::Diagnostics* diagnostics,
                                 const state_class::MetricsReport& report,
                                 bool max_states_truncated, bool horizon_truncated,
                                 size_t max_states) {
  if (diagnostics == nullptr) {
    return;
  }

  const bool deadlock = report.deadlock_analysis_certified && !report.deadlock_states.empty();
  if (deadlock) {
    diagnostics->add("R-ANA-001",
                     "reachable deadlock: " + std::to_string(report.deadlock_states.size()) +
                         " illegitimate sink state(s)",
                     "system:deadlock", std::nullopt,
                     "fix the dependency/lock structure or choose a different policy");
  }

  std::vector<std::string> local_deadlocks;
  nlohmann::json deadline_misses = nlohmann::json::array();
  for (const auto& task : report.tasks) {
    if (report.deadlock_analysis_certified && task.local_deadlock) {
      local_deadlocks.push_back(task.name);
      diagnostics->add("R-ANA-003", "task '" + task.name + "' is in a periodic local deadlock",
                       "task:" + task.name, std::nullopt,
                       "change the task's period, priority, or core assignment so it can complete");
    }
    if (report.wcrt_lb_valid && task.observed && task.has_deadline && !task.wcrt_lb.infinite &&
        task.wcrt_lb.value > task.deadline) {
      deadline_misses.push_back(
          {{"task", task.name}, {"wcrt_lb", task.wcrt_lb.value}, {"deadline", task.deadline}});
      diagnostics->add("R-ANA-002",
                       "task '" + task.name + "' has a certified response-time lower bound " +
                           std::to_string(task.wcrt_lb.value) + " exceeding its deadline " +
                           std::to_string(task.deadline),
                       "task:" + task.name, std::nullopt,
                       "adjust priorities, cores, or periods so the task can meet its deadline");
    }
  }

  const bool counterexample = deadlock || !local_deadlocks.empty() || !deadline_misses.empty();
  const bool truncated = max_states_truncated || horizon_truncated;
  if (truncated) {
    const std::string fix =
        counterexample
            ? "a counterexample was already found; raising -m cannot change the verdict; fix the "
              "model design"
            : "no counterexample found; raising -m may help";
    if (max_states_truncated) {
      diagnostics->add("E-PERF-001",
                       "state-class graph truncated at the max-states bound (" +
                           std::to_string(max_states) + ")",
                       "system:max_states", std::nullopt, fix);
    }
    if (horizon_truncated) {
      diagnostics->add("E-PERF-002",
                       "state-class graph truncated by the time horizon; some expansions were "
                       "pruned",
                       "system:horizon", std::nullopt,
                       counterexample
                           ? "a counterexample was already found; raising -m cannot change the "
                             "verdict; fix the model design"
                           : "no counterexample found; raising -m may help, or increase/disable "
                             "--horizon");
    }
  }

  std::string counterexample_kind;
  if (deadlock) {
    counterexample_kind = "deadlock";
  } else if (!local_deadlocks.empty()) {
    counterexample_kind = "local-deadlock";
  } else if (!deadline_misses.empty()) {
    counterexample_kind = "deadline";
  }

  nlohmann::json analysis = nlohmann::json::object();
  analysis["exact"] = report.exact;
  analysis["truncated"] = truncated;
  analysis["max_states"] = max_states;
  analysis["horizon_truncated"] = horizon_truncated;
  analysis["states"] = report.states;
  analysis["transitions"] = report.transitions;
  analysis["deadlock"] = deadlock;
  analysis["deadlock_states"] = report.deadlock_states.size();
  analysis["schedulable"] = report.deadlock_analysis_certified ? nlohmann::json(report.schedulable)
                                                               : nlohmann::json(nullptr);
  analysis["local_deadlocks"] = local_deadlocks;
  analysis["deadline_misses"] = std::move(deadline_misses);
  analysis["counterexample"] =
      counterexample_kind.empty() ? nlohmann::json(nullptr) : nlohmann::json(counterexample_kind);
  diagnostics->set_result(nlohmann::json{{"analysis", std::move(analysis)}});
}

bool write_wcet_json(const tdg::TDG& graph, const std::string& output_path) {
  nlohmann::json wcet = nlohmann::json::object();
  wcet["tasks"] = nlohmann::json::array();

  bool all_certified = true;
  size_t task_id = 0;
  for (const auto& node : graph.all_task) {
    if (!std::holds_alternative<TaskNode>(node)) {
      continue;
    }

    const auto& task = std::get<TaskNode>(node);
    int wcet_lo = 0;
    int wcet_hi = 0;
    for (const auto& interval : task.time) {
      wcet_lo += interval.first;
      wcet_hi += interval.second;
    }

    tdg::WcetProvenance provenance;
    if (const auto it = graph.task_wcet_provenance.find(task.name);
        it != graph.task_wcet_provenance.end()) {
      provenance = it->second;
    }
    all_certified = all_certified && provenance.kind == "certified";

    wcet["tasks"].push_back({
        {"id", task_id++},
        {"name", task.name},
        {"wcet", wcet_hi},
        {"interval", nlohmann::json::array({wcet_lo, wcet_hi})},
        {"segments", task.time.size()},
        {"wcet_kind", provenance.kind},
        {"wcet_method", provenance.method},
        {"wcet_artifact", provenance.artifact},
    });
  }
  wcet["all_certified"] = all_certified;

  std::ofstream out(output_path);
  if (!out.is_open()) {
    return false;
  }

  out << wcet.dump(2) << '\n';
  // Detect buffered/delayed write failures before reporting success.
  out.flush();
  if (!out) {
    return false;
  }
  out.close();
  if (out.fail()) {
    return false;
  }
  return true;
}

}  // namespace app
