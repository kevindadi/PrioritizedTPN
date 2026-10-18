#if defined(_WIN32)
#  include <psapi.h>
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach/mach_init.h>
#  include <mach/task.h>
#else
#  include <sys/resource.h>
#  include <unistd.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <nlohmann/json.hpp>
#include <set>
#include <spdlog/spdlog.h>
#include <variant>

#include "analysis/allocation/allocation.h"
#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reductions/cone_projection.h"
#include "app/pipeline.h"
#include "export/export_dot.h"
#include "export/export_ptpn.h"
#include "export/scg_dot.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "lower/tdg2ptopner/tdg2ptopner.h"
#include "lower/tdg2ptopner/validate.h"
#include "lower/tdg2romeo/tdg2romeo.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "parse/json.h"
#include "parse/ptpn_parser.h"

using namespace std;
namespace fs = std::filesystem;

namespace app {

namespace {

enum class InputFormat { AUTO, TDG, PTPN };

using PipelineClock = chrono::high_resolution_clock;

long long elapsed_ms(const PipelineClock::time_point& start) {
  return chrono::duration_cast<chrono::milliseconds>(PipelineClock::now() - start).count();
}

void log_step_timing(const string& step, long long ms, const string& detail = {}) {
  if (detail.empty()) {
    spdlog::info("[STATS] {}: {} ms", step, ms);
  } else {
    spdlog::info("[STATS] {}: {} ms ({})", step, ms, detail);
  }
}

bool has_any_export(const ExportTargets& exports) {
  return !exports.tdg_dot.empty() || !exports.wcet_json.empty() || !exports.ptpn_dot.empty() ||
         !exports.scg_dot.empty() || !exports.metrics_json.empty() || !exports.romeo_file.empty() ||
         !exports.ppn_file.empty() || !exports.tina_file.empty();
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

string format_allocation_assignment(
    const vector<pair<string, int>>& assignment) {
  string text;
  for (const auto& [task, core] : assignment) {
    if (!text.empty()) {
      text += ", ";
    }
    text += task + "->" + to_string(core);
  }
  return text.empty() ? "(empty)" : text;
}

const char* allocation_objective_name(allocation::Objective objective) {
  switch (objective) {
    case allocation::Objective::MAX_UTILIZATION:
      return "max-utilization";
    case allocation::Objective::MAX_LOAD:
      return "max-load";
    case allocation::Objective::MAX_WCRT:
    default:
      return "max-wcrt";
  }
}

bool should_run_analysis(const PipelineOptions& opts) {
  if (!opts.exports.scg_dot.empty() || !opts.exports.metrics_json.empty()) {
    return true;
  }
  return !opts.skip_analysis;
}

InputFormat resolve_input_format(const fs::path& input_path, const string& format_hint) {
  if (format_hint == "tdg") {
    return InputFormat::TDG;
  }
  if (format_hint == "ptpn") {
    return InputFormat::PTPN;
  }
  const string ext = input_path.extension().string();
  if (ext == ".ptpn") {
    return InputFormat::PTPN;
  }
  return InputFormat::TDG;
}

state_class::CanonicalizationMode parse_canonicalization(const string& mode) {
  if (mode == "max-lower") {
    return state_class::CanonicalizationMode::MAX_LOWER_BOUND;
  }
  if (mode == "intersection") {
    return state_class::CanonicalizationMode::INTERSECTION;
  }
  return state_class::CanonicalizationMode::EQUALITY;
}

state_class::ReductionMode parse_reduction(const string& mode) {
  if (mode == "upper-bound") {
    return state_class::ReductionMode::UPPER_BOUND;
  }
  return state_class::ReductionMode::NONE;
}

romeo::RomeoFormat romeo_format_from_string(const string& format) {
  if (format == "inhibitor-arc" || format == "inhibitor_arc") {
    return romeo::RomeoFormat::InhibitorArc;
  }
  return romeo::RomeoFormat::SchedulingNet;
}

romeo::RomeoExportOptions make_romeo_export_options(const string& format,
                                                    bool explicit_core_places) {
  romeo::RomeoExportOptions opts;
  opts.format = romeo_format_from_string(format);
  opts.explicit_core_places = explicit_core_places;
  return opts;
}

void apply_policy_override(tdg::TDG& tdg, const optional<SchedulePolicy>& policy_override) {
  if (!policy_override.has_value()) {
    return;
  }
  tdg.policy = policy_override.value();
  spdlog::info("[TDG] Scheduling policy override: {}", schedule_policy_to_string(tdg.policy));
}

int validate_ptopner_tdg(const tdg::TDG& tdg) {
  const auto ppn_validation = ptopner_export::validate_for_ptopner(tdg);
  if (!ppn_validation.ok) {
    cerr << "ERROR: PToPNer export validation failed:" << endl;
    for (const auto& err : ppn_validation.errors) {
      cerr << "  - " << err << endl;
    }
    return 1;
  }
  if (!ppn_validation.warnings.empty()) {
    cout << "PToPNer export warnings:" << endl;
    for (const auto& warn : ppn_validation.warnings) {
      cout << "  - " << warn << endl;
    }
  }
  return 0;
}

int export_romeo_from_tdg(const tdg::TDG& tdg, const string& output_path,
                          const romeo::RomeoExportOptions& opts) {
  const auto result = romeo::export_tdg_to_romeo_cts(tdg, output_path, opts);
  if (result.success) {
    spdlog::info("[OUTPUT] Romeo CTS exported to: {}", output_path);
    return 0;
  }
  cerr << "ERROR: Failed to export Romeo CTS: " << result.error_message << endl;
  return 1;
}

int export_ppn_from_ptpn(const petri::PTPN& ptpn, const string& output_path) {
  const auto ppn_export = ptopner_export::export_ptpn_to_ppn_file(ptpn, output_path);
  if (!ppn_export.success) {
    cerr << "ERROR: PToPNer export failed: " << ppn_export.error_message << endl;
    return 1;
  }
  spdlog::info("[OUTPUT] PToPNer .ppn exported to: {}", output_path);
  return 0;
}

int load_tdg_from_json(const string& input_file, tdg::TDG& tdg, parse::Parser& parser) {
  const auto parse_result = parser.parse_file(input_file);
  if (!parse_result.success) {
    cerr << "ERROR: Failed to parse JSON file: " << parse_result.error_message << endl;
    return 1;
  }

  const auto validation = parser.validate();
  if (!validation.success) {
    cerr << "ERROR: Input validation failed:" << endl;
    for (const auto& err : validation.errors) {
      cerr << "  - " << err << endl;
    }
    return 1;
  }

  if (!validation.warnings.empty()) {
    cout << "Warnings:" << endl;
    for (const auto& warn : validation.warnings) {
      cout << "  - " << warn << endl;
    }
  }

  tdg = tdg::TDG(parser.get_num_cpus(), parser.get_cores_per_cpu());
  tdg.load_from_parser(parser, /*log_nodes=*/true);
  return 0;
}

int build_ptpn_from_tdg(tdg::TDG& tdg, petri::PTPN& ptpn) {
  spdlog::info("\n[PTPN] Converting TDG to PTPN...");
  try {
    converter::TDG2PN::transform(tdg, ptpn);
  } catch (const std::exception& error) {
    spdlog::error("[PTPN] TDG to PTPN conversion failed: {}", error.what());
    return 1;
  }
  spdlog::info("[PTPN] PTPN conversion completed");
  spdlog::info("  Places: {}", ptpn.num_places());
  spdlog::info("  Transitions: {}", ptpn.num_transitions());
  spdlog::info("  Policy: {}", schedule_policy_to_string(tdg.policy));
  return 0;
}

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

  const auto cone_start = PipelineClock::now();
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
    const bool cone_exact =
        canonicalization == state_class::CanonicalizationMode::EQUALITY &&
        !opts.extrapolation && !stats.truncated;
    merged.exact = merged.exact && cone_exact;
    merged.truncated = merged.truncated || stats.truncated;
    merged.states += stats.total_states;
    merged.transitions += stats.total_transitions;

    state_class::MetricsAnalyzer analyzer(graph.get_graph(), cone,
                                          graph.get_initial_vertex(), cone_exact,
                                          /*wcrt_lb_valid=*/true);
    state_class::MetricsReport cone_report = analyzer.analyze();
    merged.bounded = merged.bounded && cone_report.bounded;
    merged.overflow_places.insert(merged.overflow_places.end(),
                                  cone_report.overflow_places.begin(),
                                  cone_report.overflow_places.end());
    merged.has_steady_cycle = merged.has_steady_cycle || cone_report.has_steady_cycle;
    merged.recurrent_scc_size =
        std::max(merged.recurrent_scc_size, cone_report.recurrent_scc_size);

    const auto metric_it =
        std::find_if(cone_report.tasks.begin(), cone_report.tasks.end(),
                     [&](const state_class::TaskMetrics& metrics) {
                       return metrics.name == task_name;
                     });
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
    metrics.jobs_per_hyperperiod =
        (metrics.period > 0 && hyperperiod > 0)
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
  result.duration_ms = elapsed_ms(cone_start);
  result.tasks = task_names.size();
  return true;
}

int run_ptpn_postprocess(const petri::PTPN& ptpn, const string& input_label,
                         const PipelineOptions& opts,
                         const chrono::time_point<chrono::high_resolution_clock>& pipeline_start,
                         size_t initial_memory) {
  if (opts.debug_mode) {
    cout << ptpn.to_string();
  }

  const auto export_model = petri::exporting::build_export_model(ptpn);

  if (!opts.exports.ptpn_dot.empty()) {
    if (petri::exporting::save_to_dot(export_model, opts.exports.ptpn_dot)) {
      spdlog::info("[OUTPUT] PTPN DOT exported to: {}", opts.exports.ptpn_dot);
    } else {
      spdlog::warn("[OUTPUT] Failed to export PTPN DOT");
      return 1;
    }
  }

  if (!opts.exports.ppn_file.empty()) {
    if (export_ppn_from_ptpn(ptpn, opts.exports.ppn_file) != 0) {
      return 1;
    }
  }

  if (!opts.exports.tina_file.empty()) {
    spdlog::info("[OUTPUT] Tina export not implemented");
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
        log_step_timing("Cone analysis", cone_result.duration_ms,
                        "tasks=" + to_string(cone_result.tasks) +
                            ", states=" + to_string(report.states) +
                            ", edges=" + to_string(report.transitions));
        if (!opts.exports.metrics_json.empty()) {
          if (!state_class::MetricsAnalyzer::save_to_json(report,
                                                          opts.exports.metrics_json)) {
            spdlog::warn("[OUTPUT] Failed to export metrics");
            return 1;
          }
          spdlog::info("[OUTPUT] Metrics exported to: {}", opts.exports.metrics_json);
          spdlog::info("[METRICS] schedulable={}, bounded={}, deadlocks={}",
                       report.schedulable ? "true" : "false",
                       report.bounded ? "true" : "false", report.deadlock_states.size());
        }
        const auto end_time = PipelineClock::now();
        const auto total_duration =
            chrono::duration_cast<chrono::milliseconds>(end_time - pipeline_start);
        const size_t total_memory = get_memory_usage() - initial_memory;
        log_step_timing(input_label + " pipeline (total)", total_duration.count(),
                        to_string(total_memory) + " KB");
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
        spdlog::warn(
            "[SCG] Reduction 'upper-bound' takes precedence over canonicalization '{}'",
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
    const auto scg_start = PipelineClock::now();
    const size_t state_count = reachability_graph.build(opts.max_states);
    const auto& reachability_stats = reachability_graph.get_statistics();
    log_step_timing("SCG build", elapsed_ms(scg_start),
                    "states=" + to_string(reachability_stats.total_states) +
                        ", edges=" + to_string(reachability_stats.total_transitions) +
                        ", dedup_hits=" + to_string(reachability_stats.dedup_hits) +
                        ", zone_merges=" + to_string(reachability_stats.zone_merges) +
                        ", merged_states=" + to_string(reachability_stats.merged_states) +
                        (reachability_stats.horizon_truncated
                             ? ", horizon_pruned=" +
                                   to_string(reachability_stats.horizon_pruned_states)
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
      const auto scg_export_start = PipelineClock::now();
      const scg_export::ScgFormatter formatter(ptpn, reachability_graph.get_graph());
      if (formatter.save_to_dot(opts.exports.scg_dot)) {
        log_step_timing("SCG DOT export", elapsed_ms(scg_export_start));
        spdlog::info("[OUTPUT] State class graph exported to: {}", opts.exports.scg_dot);
      } else {
        spdlog::warn("[OUTPUT] Failed to export state class graph");
        return 1;
      }
    }

    if (!opts.exports.metrics_json.empty()) {
      // Metrics are exact only for the complete, un-abstracted graph. Zone
      // extrapolation preserves reachable markings but relaxes timing bounds,
      // upper-bound zone merging over-approximates the zone space, truncation
      // omits part of the state space, and a horizon cut makes WCRT sound only
      // under the caller's H >= WCRT assumption. Symmetry quotients keep all
      // reachable classes, but graph edges carry representative task labels;
      // timing values are pooled within verified groups and therefore reported
      // as bounds rather than certified exact.
      const bool horizon_truncated = opts.time_horizon.has_value() &&
                                     reachability_stats.horizon_truncated;
      const bool symmetry_used = reachability_graph.symmetry_applied();
      const bool exact = canonicalization == state_class::CanonicalizationMode::EQUALITY &&
                         !opts.extrapolation && !upper_bound_merging &&
                         !reachability_stats.truncated && !horizon_truncated &&
                         !symmetry_used;
      if (!exact) {
        spdlog::warn(
            "[METRICS] Metrics are approximate (canonicalization='{}', "
            "extrapolation={}, reduction='{}', truncated={}, "
            "horizon_truncated={}, symmetry={}); timing values are bounds, not "
            "exact values",
            opts.canonicalization_mode, opts.extrapolation, opts.reduction_mode,
            reachability_stats.truncated ? "true" : "false",
            horizon_truncated ? "true" : "false",
            symmetry_used ? "true" : "false");
      }
      if (upper_bound_merging) {
        spdlog::warn(
            "[METRICS] wcrt_lb is not certified on the merged graph and is "
            "reported as unavailable (null)");
      }
      const auto metrics_start = PipelineClock::now();
      state_class::MetricsAnalyzer analyzer(reachability_graph.get_graph(), ptpn,
                                            reachability_graph.get_initial_vertex(), exact,
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
      if (state_class::MetricsAnalyzer::save_to_json(report, opts.exports.metrics_json)) {
        log_step_timing("Metrics analysis", elapsed_ms(metrics_start));
        spdlog::info("[OUTPUT] Metrics exported to: {}", opts.exports.metrics_json);
        if (report.deadlock_analysis_certified) {
          spdlog::info("[METRICS] schedulable={}, bounded={}, deadlocks={}",
                       report.schedulable ? "true" : "false",
                       report.bounded ? "true" : "false", report.deadlock_states.size());
        } else {
          spdlog::warn(
              "[METRICS] deadlock/schedulability/SCC results are not certified "
              "(time horizon truncated the state space); schedulability=unknown");
        }
      } else {
        spdlog::warn("[OUTPUT] Failed to export metrics");
        return 1;
      }
    }
  } else {
    spdlog::info("[SCG] Reachability analysis skipped");
  }

  const auto end_time = PipelineClock::now();
  const auto total_duration =
      chrono::duration_cast<chrono::milliseconds>(end_time - pipeline_start);
  const size_t total_memory = get_memory_usage() - initial_memory;
  log_step_timing(input_label + " pipeline (total)", total_duration.count(),
                  to_string(total_memory) + " KB");
  return 0;
}

}  // namespace

bool write_wcet_json(const tdg::TDG& graph, const std::string& output_path) {
  nlohmann::json wcet = nlohmann::json::object();
  wcet["tasks"] = nlohmann::json::array();

  bool all_certified = true;
  size_t task_id = 0;
  for (const auto& node : graph.all_task) {
    if (!holds_alternative<TaskNode>(node)) {
      continue;
    }

    const auto& task = get<TaskNode>(node);
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

  ofstream out(output_path);
  if (!out.is_open()) {
    return false;
  }

  out << wcet.dump(2) << '\n';
  return true;
}

size_t get_memory_usage() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return pmc.WorkingSetSize / 1024 / 1024;
  }
  return 0;
#elif defined(__APPLE__)
  const task_t task = mach_task_self();
  struct task_basic_info t_info{};
  mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;
  if (task_info(task, TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&t_info), &t_info_count) ==
      KERN_SUCCESS) {
    return t_info.resident_size / 1024 / 1024;
  }
  return 0;
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    return usage.ru_maxrss / 1024;
  }
  return 0;
#endif
}

int run_tdg_pipeline(const string& input_file, PipelineOptions opts, size_t initial_memory) {
  const auto pipeline_start = PipelineClock::now();
  const auto tdg_start = PipelineClock::now();
  spdlog::info("[TDG] Loading JSON: {}", input_file);

  parse::Parser parser;
  tdg::TDG tdg(1, 1);
  if (load_tdg_from_json(input_file, tdg, parser) != 0) {
    return 1;
  }

  apply_policy_override(tdg, opts.policy_override);

  spdlog::info("[TDG] Configuration: {} CPUs, {} cores per CPU, policy={}", parser.get_num_cpus(),
               parser.get_cores_per_cpu(), schedule_policy_to_string(tdg.policy));

  if (!opts.exports.tdg_dot.empty()) {
    tdg.export_to_dot(opts.exports.tdg_dot);
    spdlog::info("[OUTPUT] TDG DOT exported to: {}", opts.exports.tdg_dot);
  }

  if (!opts.exports.wcet_json.empty()) {
    if (!write_wcet_json(tdg, opts.exports.wcet_json)) {
      spdlog::warn("[OUTPUT] Failed to save WCET JSON to: {}", opts.exports.wcet_json);
      return 1;
    }
    spdlog::info("[OUTPUT] WCET JSON exported to: {}", opts.exports.wcet_json);
  }

  const auto tdg_end = PipelineClock::now();
  const size_t tdg_memory = get_memory_usage() - initial_memory;
  log_step_timing("TDG parsing", elapsed_ms(tdg_start), to_string(tdg_memory) + " KB");

  if (!opts.exports.ppn_file.empty()) {
    if (validate_ptopner_tdg(tdg) != 0) {
      return 1;
    }
  }

  if (!opts.exports.romeo_file.empty()) {
    const auto romeo_opts =
        make_romeo_export_options(opts.romeo_format, opts.romeo_explicit_core_places);
    if (export_romeo_from_tdg(tdg, opts.exports.romeo_file, romeo_opts) != 0) {
      return 1;
    }
  }

  const bool needs_ptpn = should_run_analysis(opts) || !opts.exports.ptpn_dot.empty() ||
                          !opts.exports.ppn_file.empty() || !opts.exports.scg_dot.empty() ||
                          !opts.exports.metrics_json.empty();

  if (!needs_ptpn) {
    if (!has_any_export(opts.exports)) {
      spdlog::warn("[MAIN] No exports requested and analysis disabled; nothing to do");
      return 0;
    }
    const auto end_time = PipelineClock::now();
    log_step_timing("TDG pipeline (total)", elapsed_ms(pipeline_start),
                    to_string(get_memory_usage() - initial_memory) + " KB");
    return 0;
  }

  petri::PTPN ptpn;
  const auto lowering_start = PipelineClock::now();
  if (build_ptpn_from_tdg(tdg, ptpn) != 0) {
    return 1;
  }
  log_step_timing("TDG2PN lowering", elapsed_ms(lowering_start));

  return run_ptpn_postprocess(ptpn, "TDG", opts, pipeline_start, initial_memory);
}

int run_ptpn_pipeline(const string& input_file, const PipelineOptions& opts,
                      size_t initial_memory) {
  const auto pipeline_start = PipelineClock::now();
  const auto parse_start = PipelineClock::now();
  spdlog::info("[PTPN] Loading source: {}", input_file);

  const auto parse_result = parser::PTPNBuilder::parse_file(input_file);
  if (!parse_result.ok()) {
    cerr << "ERROR: Failed to parse PTPN file: " << parse_result.error() << endl;
    return 1;
  }
  const petri::PTPN& ptpn = parse_result.value();

  log_step_timing("PTPN parsing", elapsed_ms(parse_start));

  spdlog::info("[PTPN] Parse completed");
  spdlog::info("  Places: {}", ptpn.num_places());
  spdlog::info("  Transitions: {}", ptpn.num_transitions());

  if (!should_run_analysis(opts) && !has_any_export(opts.exports)) {
    spdlog::warn("[MAIN] No exports requested and analysis disabled; nothing to do");
    return 0;
  }

  return run_ptpn_postprocess(ptpn, "PTPN", opts, pipeline_start, initial_memory);
}

int run_export_romeo(const ExportCommandOptions& opts) {
  const fs::path input_path(opts.input_file);
  if (opts.input_format == "ptpn" ||
      (opts.input_format == "auto" && input_path.extension() == ".ptpn")) {
    cerr << "ERROR: Romeo export accepts TDG JSON only (use tdg2romeo, not PTPN input)" << endl;
    return 1;
  }

  spdlog::info("[EXPORT] Romeo from TDG JSON: {}", opts.input_file);
  parse::Parser parser;
  tdg::TDG tdg(1, 1);
  if (load_tdg_from_json(opts.input_file, tdg, parser) != 0) {
    return 1;
  }

  const auto romeo_opts =
      make_romeo_export_options(opts.romeo_format, opts.romeo_explicit_core_places);
  return export_romeo_from_tdg(tdg, opts.output_file, romeo_opts);
}

int run_export_ptopner(const ExportCommandOptions& opts) {
  const fs::path input_path(opts.input_file);
  const InputFormat format = resolve_input_format(input_path, opts.input_format);

  if (format == InputFormat::PTPN) {
    spdlog::info("[EXPORT] PToPNer from PTPN source: {}", opts.input_file);
    const auto ptpn_result = parser::PTPNBuilder::parse_file(opts.input_file);
    if (!ptpn_result.ok()) {
      cerr << "ERROR: Failed to parse PTPN file: " << ptpn_result.error() << endl;
      return 1;
    }
    return export_ppn_from_ptpn(ptpn_result.value(), opts.output_file);
  }

  spdlog::info("[EXPORT] PToPNer from TDG JSON: {}", opts.input_file);
  parse::Parser parser;
  tdg::TDG tdg(1, 1);
  if (load_tdg_from_json(opts.input_file, tdg, parser) != 0) {
    return 1;
  }

  if (opts.policy_override.has_value()) {
    if (opts.policy_override.value() != SchedulePolicy::FIXED_PRIOR_WITH_RESTART) {
      cerr << "ERROR: PToPNer export requires fixed_prior_with_restart policy" << endl;
      return 1;
    }
    tdg.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;
  } else if (tdg.policy != SchedulePolicy::FIXED_PRIOR_WITH_RESTART) {
    spdlog::info(
        "[EXPORT] Overriding TDG policy {} -> fixed_prior_with_restart for "
        "PToPNer",
        schedule_policy_to_string(tdg.policy));
    tdg.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;
  }

  if (validate_ptopner_tdg(tdg) != 0) {
    return 1;
  }

  petri::PTPN ptpn;
  if (build_ptpn_from_tdg(tdg, ptpn) != 0) {
    return 1;
  }
  return export_ppn_from_ptpn(ptpn, opts.output_file);
}

int run_allocation(const AllocationCommandOptions& opts) {
  spdlog::info("[ALLOC] Loading TDG JSON: {}", opts.input_file);

  parse::Parser parser;
  tdg::TDG tdg(1, 1);
  if (load_tdg_from_json(opts.input_file, tdg, parser) != 0) {
    return 1;
  }
  apply_policy_override(tdg, opts.policy_override);

  allocation::Options search_options;
  search_options.max_candidates = opts.max_candidates;
  search_options.candidate_max_states = opts.candidate_max_states;
  search_options.objective = opts.objective;
  search_options.require_certified_wcet = opts.require_certified_wcet;

  allocation::SearchReport report;
  {
    // Per-candidate lowering/analysis logs at info level are suppressed unless
    // --debug was requested. The final summary below is logged after the guard
    // restores the previous level.
    optional<ScopedLogLevel> quiet;
    if (!opts.debug_mode) {
      quiet.emplace(spdlog::level::warn);
    }
    report = allocation::search_allocations(tdg, search_options);
  }

  if (!allocation::save_report_json(report, opts.output_file)) {
    cerr << "ERROR: Failed to write allocation report: " << opts.output_file << endl;
    return 1;
  }

  spdlog::info("[ALLOC] Strategy: {} (complete={}), objective={}",
               report.strategy == allocation::Strategy::EXHAUSTIVE ? "exhaustive" : "heuristic",
               report.complete ? "true" : "false",
               allocation_objective_name(report.objective));
  spdlog::info("[ALLOC] Domain size: {}, evaluated: {}, feasible: {}{}", report.domain_size,
               report.evaluated, report.feasible_count,
               report.truncated ? " (search truncated)" : "");
  if (report.best.has_value()) {
    const allocation::CandidateSummary& best = report.best.value();
    spdlog::info("[ALLOC] Best assignment: {}",
                 format_allocation_assignment(best.assignment));
    if (best.max_wcrt_infinite) {
      spdlog::info("[ALLOC] max_wcrt: infinite, max_core_load: {}", best.max_core_load);
    } else {
      spdlog::info("[ALLOC] max_wcrt: {}, max_core_load: {}", best.max_wcrt,
                   best.max_core_load);
    }
    if (best.utilization_complete) {
      spdlog::info("[ALLOC] max_utilization: {:.6f}", best.max_utilization);
    }
    spdlog::info("[ALLOC] Pareto allocations: {}", report.pareto.size());
  } else {
    spdlog::info("[ALLOC] No best allocation: {}", report.reason);
  }
  spdlog::info("[OUTPUT] Allocation report exported to: {}", opts.output_file);
  return 0;
}

}  // namespace app
