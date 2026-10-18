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
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <variant>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
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

bool write_wcet_json(const tdg::TDG& tdg, const fs::path& output_path) {
  nlohmann::json wcet = nlohmann::json::object();
  wcet["tasks"] = nlohmann::json::array();

  size_t task_id = 0;
  for (const auto& node : tdg.all_task) {
    if (!holds_alternative<TaskNode>(node)) {
      continue;
    }

    const auto& task = get<TaskNode>(node);
    int task_wcet = 0;
    for (const auto& interval : task.time) {
      task_wcet += interval.second;
    }

    wcet["tasks"].push_back({
        {"id", task_id++},
        {"name", task.name},
        {"wcet", task_wcet},
        {"segments", task.time.size()},
    });
  }

  ofstream out(output_path);
  if (!out.is_open()) {
    return false;
  }

  out << wcet.dump(2) << '\n';
  return true;
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
  converter::TDG2PN::transform(tdg, ptpn);
  spdlog::info("[PTPN] PTPN conversion completed");
  spdlog::info("  Places: {}", ptpn.num_places());
  spdlog::info("  Transitions: {}", ptpn.num_transitions());
  spdlog::info("  Policy: {}", schedule_policy_to_string(tdg.policy));
  return 0;
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

  if (should_run_analysis(opts)) {
    const auto canonicalization = parse_canonicalization(opts.canonicalization_mode);
    state_class::StateClassReachabilityGraph reachability_graph(ptpn);
    reachability_graph.set_canonicalization_mode(canonicalization);
    spdlog::info("[SCG] Canonicalization mode: {}", opts.canonicalization_mode);
    if (opts.extrapolation) {
      reachability_graph.set_extrapolation(true);
      spdlog::info("[SCG] k-extrapolation enabled (k={})",
                   reachability_graph.extrapolation_bound());
    }
    const auto scg_start = PipelineClock::now();
    const size_t state_count = reachability_graph.build(opts.max_states);
    const auto& reachability_stats = reachability_graph.get_statistics();
    log_step_timing("SCG build", elapsed_ms(scg_start),
                    "states=" + to_string(reachability_stats.total_states) +
                        ", edges=" + to_string(reachability_stats.total_transitions) +
                        ", dedup_hits=" + to_string(reachability_stats.dedup_hits) +
                        (reachability_stats.truncated ? ", truncated" : ""));
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
      const bool exact = canonicalization == state_class::CanonicalizationMode::EQUALITY;
      if (!exact) {
        spdlog::warn(
            "[METRICS] Canonicalization '{}' merges state classes; metrics are "
            "approximate. Use --canonicalization equality for sound bounds",
            opts.canonicalization_mode);
      }
      const auto metrics_start = PipelineClock::now();
      state_class::MetricsAnalyzer analyzer(reachability_graph.get_graph(), ptpn,
                                            reachability_graph.get_initial_vertex(), exact);
      const state_class::MetricsReport report = analyzer.analyze();
      if (state_class::MetricsAnalyzer::save_to_json(report, opts.exports.metrics_json)) {
        log_step_timing("Metrics analysis", elapsed_ms(metrics_start));
        spdlog::info("[OUTPUT] Metrics exported to: {}", opts.exports.metrics_json);
        spdlog::info("[METRICS] schedulable={}, bounded={}, deadlocks={}",
                     report.schedulable ? "true" : "false", report.bounded ? "true" : "false",
                     report.deadlock_states.size());
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
  build_ptpn_from_tdg(tdg, ptpn);
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
  build_ptpn_from_tdg(tdg, ptpn);
  return export_ppn_from_ptpn(ptpn, opts.output_file);
}

}  // namespace app
