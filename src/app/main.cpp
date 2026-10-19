#include <CLI/CLI.hpp>
#include <cstddef>
#include <iostream>
#include <spdlog/spdlog.h>
#include <string>

#include "app/pipeline.h"
#include "model/types.h"

using namespace std;

namespace {

#if defined(PTPN_ENABLE_COMPAT)
const char* const kCompatExportNote = "";
#else
const char* const kCompatExportNote = " (not built; rebuild with -DPTPN_ENABLE_COMPAT=ON)";
#endif

#if !defined(PTPN_ENABLE_COMPAT)
int report_export_unavailable(const string& feature) {
  cerr << "ERROR: " << feature
       << " export is not available: built without compat support "
          "(rebuild with -DPTPN_ENABLE_COMPAT=ON)"
       << endl;
  return 1;
}

int reject_compat_exports(const app::ExportTargets& exports) {
  if (exports.romeo_file.empty() && exports.ppn_file.empty()) {
    return 0;
  }
  cerr << "ERROR: Romeo/PToPNer export is not available: built without compat "
          "support (rebuild with -DPTPN_ENABLE_COMPAT=ON)"
       << endl;
  return 1;
}
#endif

SchedulePolicy parse_policy_option(const string& policy) {
  const SchedulePolicy parsed = parse_schedule_policy(policy);
  if (parsed == SchedulePolicy::UNKNOWN) {
    throw CLI::ValidationError(policy,
                               "Expected fixed, fixed_prior_with_restart, fixed_prior_with_resume, "
                               "fifo, rm, dm, edf, llf, pip, pcp, or srp");
  }
  return parsed;
}

allocation::Objective parse_objective_option(const string& objective) {
  if (objective == "max-utilization") {
    return allocation::Objective::MAX_UTILIZATION;
  }
  if (objective == "max-load") {
    return allocation::Objective::MAX_LOAD;
  }
  return allocation::Objective::MAX_WCRT;
}

void add_export_target_options(CLI::App* cmd, app::ExportTargets& exports) {
  cmd->add_option("--export-tdg", exports.tdg_dot, "Write TDG Graphviz DOT to PATH");
  cmd->add_option("--export-wcet", exports.wcet_json, "Write per-task WCET summary JSON to PATH");
  cmd->add_option("--export-ptpn", exports.ptpn_dot, "Write PTPN structure Graphviz DOT to PATH");
  cmd->add_option("--export-scg", exports.scg_dot,
                  "Write state-class reachability graph DOT to PATH");
  cmd->add_option("--export-metrics", exports.metrics_json,
                  "Write performance metrics JSON to PATH");
  cmd->add_option("--romeo", exports.romeo_file,
                  string("Export Romeo CTS to PATH") + kCompatExportNote);
  cmd->add_option("--ppn", exports.ppn_file,
                  string("Export PToPNer .ppn to PATH") + kCompatExportNote);
  cmd->add_option("--tina", exports.tina_file, "Export Tina .net to PATH (not implemented)");
}

void add_common_pipeline_options(CLI::App* cmd, app::PipelineOptions& opts) {
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
  add_export_target_options(cmd, opts.exports);
}

void configure_export_subcommand(CLI::App* cmd, app::ExportCommandOptions& opts,
                                 const string& footer) {
  cmd->add_option("-f,--file", opts.input_file, "Input TDG JSON file")->required(true);
  cmd->add_option("-o,--output", opts.output_file, "Output file path")->required(true);
  cmd->add_option("--from", opts.input_format, "Input format: tdg (default: tdg)")
      ->check(CLI::IsMember({"tdg", "auto"}));
  cmd->add_flag("--debug", opts.debug_mode, "Enable debug logging");
  cmd->footer(footer);
}

}  // namespace

int main(int argc, char* argv[]) {
  CLI::App app{"PTPN - Priority Timed Petri Net Analyzer"};
  app.set_version_flag("-v,--version", "1.0.0");
  app.require_subcommand(1);
  app.footer(
      "Usage:\n"
      "  ptpn tdg -f <input.json> [--export-scg graph.dot] ...\n"
      "  ptpn ptpn -f <model.ptpn> [--romeo out.cts] ...\n"
      "  ptpn allocate -f <input.json> -o <report.json> [--objective ...]\n"
      "  ptpn export romeo -f <input> -o <out.cts> [--policy ...]\n"
      "  ptpn export ptopner -f <input> -o <out.ppn>\n"
      "\n"
      "File exports are opt-in. Run 'ptpn <subcommand> -h' for details.");

  app::PipelineOptions tdg_opts;
  app::PipelineOptions ptpn_opts;
  app::ExportCommandOptions romeo_export_opts;
  app::ExportCommandOptions ptopner_export_opts;
  app::AllocationCommandOptions allocation_opts;
  string tdg_file;
  string ptpn_file;
  string allocation_objective = "max-wcrt";
  string tdg_policy_string;
  string ptopner_policy_string;
  string allocation_policy_string;
  bool romeo_no_core_places = false;
  bool export_romeo_no_core_places = false;

  auto* tdg_cmd = app.add_subcommand("tdg", "Analyze from TDG JSON input");
  tdg_cmd->add_option("-f,--file", tdg_file, "Input TDG JSON file")->required(true);
  add_common_pipeline_options(tdg_cmd, tdg_opts);
  tdg_cmd
      ->add_option("--format", tdg_opts.romeo_format,
                   "Romeo export format: scheduling-net or inhibitor-arc "
                   "(default: scheduling-net)")
      ->check(CLI::IsMember({"scheduling-net", "inhibitor-arc"}));
  tdg_cmd->add_flag("--romeo-no-core-places", romeo_no_core_places,
                    "Omit explicit core resource places in Romeo export");
  tdg_cmd
      ->add_option("--policy", tdg_policy_string,
                   "Override TDG scheduling policy for lowering "
                   "(fixed_prior_with_resume for native analysis, "
                   "fixed_prior_with_restart for PToPNer)")
      ->transform([&tdg_opts](const string& value) {
        tdg_opts.policy_override = parse_policy_option(value);
        return value;
      });
  tdg_cmd->footer(
      "Examples:\n"
      "  ptpn tdg -f example/common/input.json\n"
      "  ptpn tdg -f input.json --export-scg scg.dot --export-ptpn-dot "
      "net.dot\n"
      "  ptpn tdg -f input.json --policy fixed_prior_with_resume --romeo "
      "out.cts\n"
      "  ptpn tdg -f input.json --policy fixed_prior_with_restart --ppn "
      "out.ppn\n"
      "  ptpn tdg -f input.json --no-analysis --export-wcet wcet.json\n"
      "  ptpn tdg -f input.json --romeo out.cts --format scheduling-net");

  auto* ptpn_cmd = app.add_subcommand("ptpn", "Analyze from PTPN source file");
  ptpn_cmd->add_option("-f,--file", ptpn_file, "Input .ptpn source file")->required(true);
  add_common_pipeline_options(ptpn_cmd, ptpn_opts);
  ptpn_cmd->footer(
      "Examples:\n"
      "  ptpn ptpn -f example/common/simple.ptpn\n"
      "  ptpn ptpn -f model.ptpn --export-scg scg.dot --canonicalization "
      "max-lower\n"
      "  ptpn ptpn -f model.ptpn --no-analysis --export-ptpn net.dot");

  auto* allocate_cmd = app.add_subcommand(
      "allocate", "Search deterministic core allocations with the exact PTPN analysis");
  allocate_cmd->add_option("-f,--file", allocation_opts.input_file, "Input TDG JSON file")
      ->required(true);
  allocate_cmd
      ->add_option("-o,--output", allocation_opts.output_file, "Output allocation report JSON")
      ->required(true);
  allocate_cmd
      ->add_option("--objective", allocation_objective,
                   "Objective to minimize: max-wcrt, max-utilization, or max-load "
                   "(default: max-wcrt)")
      ->check(CLI::IsMember({"max-wcrt", "max-utilization", "max-load"}))
      ->transform([&allocation_opts](const string& value) {
        allocation_opts.objective = parse_objective_option(value);
        return value;
      });
  allocate_cmd
      ->add_option("--max-candidates", allocation_opts.max_candidates,
                   "Maximum number of candidate assignments analyzed (default: 64)")
      ->check(CLI::Range(std::size_t{1}, std::size_t{1000000}));
  allocate_cmd
      ->add_option("--candidate-max-states", allocation_opts.candidate_max_states,
                   "State-class graph bound per candidate (default: 200000)")
      ->check(CLI::PositiveNumber);
  allocate_cmd->add_flag("--require-certified-wcet", allocation_opts.require_certified_wcet,
                         "Only consider allocations whose WCET provenance is certified");
  allocate_cmd->add_flag("--debug", allocation_opts.debug_mode,
                         "Enable debug logging (including per-candidate analysis logs)");
  allocate_cmd
      ->add_option("--policy", allocation_policy_string,
                   "Override the TDG scheduling policy before allocation analysis")
      ->transform([&allocation_opts](const string& value) {
        allocation_opts.policy_override = parse_policy_option(value);
        return value;
      });
  allocate_cmd->footer(
      "Examples:\n"
      "  ptpn allocate -f input.json -o allocation.json\n"
      "  ptpn allocate -f input.json -o allocation.json --objective max-load\n"
      "  ptpn allocate -f input.json -o allocation.json --max-candidates 32 "
      "--candidate-max-states 50000\n"
      "  ptpn allocate -f input.json -o allocation.json --require-certified-wcet\n"
      "\n"
      "The search evaluates candidate assignments with the exact state-class "
      "analysis and reports feasible and Pareto allocations. Fails (exit 1) only "
      "on input or output errors; an empty best allocation is reported in JSON.");

  auto* export_cmd = app.add_subcommand(
      "export", string("Export to Romeo or PToPNer formats") + kCompatExportNote);
  export_cmd->require_subcommand(1);

  auto* export_romeo_cmd =
      export_cmd->add_subcommand("romeo", "Export Romeo CTS directly from TDG JSON (tdg2romeo)");
  configure_export_subcommand(
      export_romeo_cmd, romeo_export_opts,
      "Examples:\n"
      "  ptpn export romeo -f example/p-bench/initial.json -o out.cts\n"
      "  ptpn export romeo -f input.json -o out.cts --format inhibitor-arc");
  export_romeo_cmd
      ->add_option("--format", romeo_export_opts.romeo_format,
                   "Romeo format: scheduling-net or inhibitor-arc (default: scheduling-net)")
      ->check(CLI::IsMember({"scheduling-net", "inhibitor-arc"}));
  export_romeo_cmd->add_flag("--romeo-no-core-places", export_romeo_no_core_places,
                             "Omit explicit core resource places in Romeo export");

  auto* export_ptopner_cmd = export_cmd->add_subcommand(
      "ptopner", "Export PToPNer .ppn (restart-style TDG lowering required)");
  configure_export_subcommand(export_ptopner_cmd, ptopner_export_opts,
                              "Examples:\n"
                              "  ptpn export ptopner -f example/common/input.json -o out.ppn\n"
                              "  ptpn export ptopner -f input.json -o out.ppn "
                              "--policy fixed_prior_with_restart\n"
                              "  ptpn export ptopner -f model.ptpn -o out.ppn --from ptpn");
  export_ptopner_cmd
      ->add_option("--policy", ptopner_policy_string,
                   "Must be fixed_prior_with_restart when exporting from TDG JSON")
      ->transform([&ptopner_export_opts](const string& value) {
        ptopner_export_opts.policy_override = parse_policy_option(value);
        return value;
      });

  CLI11_PARSE(app, argc, argv);

  bool debug_mode = false;
  if (tdg_cmd->parsed()) {
    debug_mode = tdg_opts.debug_mode;
  } else if (ptpn_cmd->parsed()) {
    debug_mode = ptpn_opts.debug_mode;
  } else if (allocate_cmd->parsed()) {
    debug_mode = allocation_opts.debug_mode;
  } else if (export_romeo_cmd->parsed()) {
    debug_mode = romeo_export_opts.debug_mode;
  } else if (export_ptopner_cmd->parsed()) {
    debug_mode = ptopner_export_opts.debug_mode;
  }

  if (debug_mode) {
    spdlog::set_level(spdlog::level::debug);
    spdlog::debug("[MAIN] Debug logging enabled");
  }

  spdlog::info("==========================================");
  spdlog::info("PTPN - Priority Timed Petri Net Analyzer");
  spdlog::info("==========================================");

  const size_t initial_memory = app::get_memory_usage();

  if (tdg_cmd->parsed()) {
    tdg_opts.romeo_explicit_core_places = !romeo_no_core_places;
#if !defined(PTPN_ENABLE_COMPAT)
    if (reject_compat_exports(tdg_opts.exports) != 0) {
      return 1;
    }
#endif
    return app::run_tdg_pipeline(tdg_file, tdg_opts, initial_memory);
  }
  if (ptpn_cmd->parsed()) {
#if !defined(PTPN_ENABLE_COMPAT)
    if (reject_compat_exports(ptpn_opts.exports) != 0) {
      return 1;
    }
#endif
    return app::run_ptpn_pipeline(ptpn_file, ptpn_opts, initial_memory);
  }
  if (allocate_cmd->parsed()) {
    return app::run_allocation(allocation_opts);
  }
  if (export_romeo_cmd->parsed()) {
#if defined(PTPN_ENABLE_COMPAT)
    romeo_export_opts.romeo_explicit_core_places = !export_romeo_no_core_places;
    return app::run_export_romeo(romeo_export_opts);
#else
    return report_export_unavailable("Romeo");
#endif
  }
  if (export_ptopner_cmd->parsed()) {
#if defined(PTPN_ENABLE_COMPAT)
    return app::run_export_ptopner(ptopner_export_opts);
#else
    return report_export_unavailable("PToPNer");
#endif
  }

  return 1;
}
