#include <CLI/CLI.hpp>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>

#include "app/check_cmd.h"
#include "app/pipeline.h"
#include "common/diagnostics.h"
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

int reject_compat_exports(const app::ExportTargets& exports, ptpn::Diagnostics* diagnostics) {
  if (exports.romeo_file.empty() && exports.ppn_file.empty()) {
    return 0;
  }
  const string message =
      "Romeo/PToPNer export is not available: built without compat support "
      "(rebuild with -DPTPN_ENABLE_COMPAT=ON)";
  cerr << "ERROR: " << message << endl;
  if (diagnostics != nullptr) {
    diagnostics->add("E-UNS-002", message, "policy:compat", std::nullopt,
                     "rebuild with -DPTPN_ENABLE_COMPAT=ON or drop --romeo/--ppn");
  }
  return 1;
}
#endif

// When the diagnostics envelope is active, stdout carries only the envelope;
// human-readable logs move to stderr (the [STATS] line format is unchanged).
void redirect_logs_to_stderr() {
  auto logger = spdlog::stderr_color_mt("ptpn-diagnostics");
  spdlog::set_default_logger(std::move(logger));
}

void add_diagnostics_options(CLI::App* cmd, app::DiagnosticsOptions& diagnostics) {
  cmd->add_option("--diagnostics", diagnostics.output,
                  "Emit the versioned diagnostics JSON envelope on stdout (json)")
      ->check(CLI::IsMember({"json"}));
  cmd->add_option("--diagnostics-file", diagnostics.file,
                  "Write the versioned diagnostics JSON envelope to PATH");
}

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
  add_diagnostics_options(cmd, opts.diagnostics);
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

int run_cli(int argc, char* argv[], ptpn::Diagnostics& diagnostics, bool& diagnostics_engaged,
            bool& diagnostics_stdout, std::string& diagnostics_file, std::streambuf*& saved_cout) {
  CLI::App app{"PTPN - Priority Timed Petri Net Analyzer"};
  app.set_version_flag("-v,--version", "1.0.0");
  app.require_subcommand(1);
  app.footer(
      "Usage:\n"
      "  ptpn tdg -f <input.json> [--export-scg graph.dot] ...\n"
      "  ptpn ptpn -f <model.ptpn> [--romeo out.cts] ...\n"
      "  ptpn allocate -f <input.json> -o <report.json> [--objective ...]\n"
      "  ptpn check -f <model.rtsl> [--diagnostics json]\n"
      "  ptpn export romeo -f <input> -o <out.cts> [--policy ...]\n"
      "  ptpn export ptopner -f <input> -o <out.ppn>\n"
      "\n"
      "File exports are opt-in. Run 'ptpn <subcommand> -h' for details.");

  app::PipelineOptions tdg_opts;
  app::PipelineOptions ptpn_opts;
  app::ExportCommandOptions romeo_export_opts;
  app::ExportCommandOptions ptopner_export_opts;
  app::AllocationCommandOptions allocation_opts;
  app::CheckCommandOptions check_opts;
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
  add_diagnostics_options(allocate_cmd, allocation_opts.diagnostics);
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

  auto* check_cmd = app.add_subcommand(
      "check", "Lex, parse, and validate an RTSL model (no lowering or analysis)");
  check_cmd->add_option("-f,--file", check_opts.input_file, "Input RTSL file")->required(true);
  add_diagnostics_options(check_cmd, check_opts.diagnostics);
  check_cmd->footer(
      "Examples:\n"
      "  ptpn check -f model.rtsl\n"
      "  ptpn check -f model.rtsl --diagnostics json\n"
      "  ptpn check -f model.rtsl --diagnostics-file diagnostics.json\n"
      "\n"
      "Successful checks report `pending_measurements` for `measured by llm`\n"
      "tasks whose intervals have not been injected yet.");

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
    diagnostics_engaged = tdg_opts.diagnostics.engaged();
    diagnostics_stdout = tdg_opts.diagnostics.output == "json";
    diagnostics_file = tdg_opts.diagnostics.file;
  } else if (ptpn_cmd->parsed()) {
    debug_mode = ptpn_opts.debug_mode;
    diagnostics_engaged = ptpn_opts.diagnostics.engaged();
    diagnostics_stdout = ptpn_opts.diagnostics.output == "json";
    diagnostics_file = ptpn_opts.diagnostics.file;
  } else if (allocate_cmd->parsed()) {
    debug_mode = allocation_opts.debug_mode;
    diagnostics_engaged = allocation_opts.diagnostics.engaged();
    diagnostics_stdout = allocation_opts.diagnostics.output == "json";
    diagnostics_file = allocation_opts.diagnostics.file;
  } else if (check_cmd->parsed()) {
    diagnostics_engaged = check_opts.diagnostics.engaged();
    diagnostics_stdout = check_opts.diagnostics.output == "json";
    diagnostics_file = check_opts.diagnostics.file;
  } else if (export_romeo_cmd->parsed()) {
    debug_mode = romeo_export_opts.debug_mode;
  } else if (export_ptopner_cmd->parsed()) {
    debug_mode = ptopner_export_opts.debug_mode;
  }

  if (diagnostics_engaged) {
    // Keep stdout reserved for the envelope; logs (including [STATS]) and any
    // stray direct stdout writes are redirected to stderr.
    redirect_logs_to_stderr();
    saved_cout = std::cout.rdbuf(std::cerr.rdbuf());
  }

  if (debug_mode) {
    spdlog::set_level(spdlog::level::debug);
    spdlog::debug("[MAIN] Debug logging enabled");
  }

  spdlog::info("==========================================");
  spdlog::info("PTPN - Priority Timed Petri Net Analyzer");
  spdlog::info("==========================================");

  const size_t initial_memory = app::get_memory_usage();
  ptpn::Diagnostics* diag = diagnostics_engaged ? &diagnostics : nullptr;

  if (tdg_cmd->parsed()) {
    tdg_opts.romeo_explicit_core_places = !romeo_no_core_places;
#if !defined(PTPN_ENABLE_COMPAT)
    if (reject_compat_exports(tdg_opts.exports, diag) != 0) {
      return 1;
    }
#endif
    return app::run_tdg_pipeline(tdg_file, tdg_opts, initial_memory, diag);
  }
  if (ptpn_cmd->parsed()) {
#if !defined(PTPN_ENABLE_COMPAT)
    if (reject_compat_exports(ptpn_opts.exports, diag) != 0) {
      return 1;
    }
#endif
    return app::run_ptpn_pipeline(ptpn_file, ptpn_opts, initial_memory, diag);
  }
  if (allocate_cmd->parsed()) {
    return app::run_allocation(allocation_opts, diag);
  }
  if (check_cmd->parsed()) {
    return app::run_rtsl_check(check_opts, diag);
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

int main(int argc, char* argv[]) {
  ptpn::Diagnostics diagnostics;
  bool diagnostics_engaged = false;
  bool diagnostics_stdout = false;
  std::string diagnostics_file;
  std::streambuf* saved_cout = nullptr;
  int result_code = 1;

  try {
    result_code = run_cli(argc, argv, diagnostics, diagnostics_engaged, diagnostics_stdout,
                          diagnostics_file, saved_cout);
  } catch (const std::exception& e) {
    if (diagnostics_engaged) {
      diagnostics.add("E-INT-001", e.what(), "system:internal", std::nullopt,
                      "report this as an internal error");
    }
    std::cerr << "ERROR: " << e.what() << std::endl;
    result_code = 1;
  } catch (...) {
    if (diagnostics_engaged) {
      diagnostics.add("E-INT-001", "unknown internal error", "system:internal", std::nullopt,
                      "report this as an internal error");
    }
    std::cerr << "ERROR: unknown internal error" << std::endl;
    result_code = 1;
  }

  if (saved_cout != nullptr) {
    std::cout.rdbuf(saved_cout);
  }

  if (!diagnostics_engaged) {
    return result_code;
  }

  // The envelope is the machine-readable outcome: failures without a specific
  // diagnostic still surface as an internal error rather than a false success.
  if (result_code != 0 && diagnostics.exit_code() == 0) {
    diagnostics.add("E-INT-001", "operation failed with exit code " + std::to_string(result_code),
                    "system:internal", std::nullopt,
                    "report this failure; it has no specific diagnostic");
  }

  // Open the file destination before serializing so a write failure is part of
  // the same envelope that would have been written.
  std::ofstream envelope_file;
  if (!diagnostics_file.empty()) {
    envelope_file.open(diagnostics_file);
    if (!envelope_file) {
      std::cerr << "ERROR: Failed to write diagnostics file: " << diagnostics_file << std::endl;
      diagnostics.add("E-INT-002", "failed to write diagnostics file: " + diagnostics_file,
                      "output:" + diagnostics_file, std::nullopt,
                      "check that the output path is writable");
    }
  }

  int code = diagnostics.exit_code();
  const nlohmann::json envelope = diagnostics.to_json();
  if (diagnostics_stdout) {
    std::cout << envelope.dump(2) << std::endl;
  }
  if (envelope_file) {
    envelope_file << envelope.dump(2) << std::endl;
  }
  return code;
}
