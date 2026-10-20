#include "app/allocation_cmd.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "app/common.h"
#include "app/input.h"
#include "app/logging.h"
#include "common/diagnostics.h"
#include "model/tdg.h"
#include "parse/json.h"

namespace app {

namespace fs = std::filesystem;

namespace {

std::string format_allocation_assignment(
    const std::vector<std::pair<std::string, int>>& assignment) {
  std::string text;
  for (const auto& [task, core] : assignment) {
    if (!text.empty()) {
      text += ", ";
    }
    text += task + "->" + std::to_string(core);
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

}  // namespace

int run_allocation(const AllocationCommandOptions& opts, ptpn::Diagnostics* diagnostics) {
  tdg::TDG tdg(1, 1);
  const bool is_rtsl = fs::path(opts.input_file).extension() == ".rtsl";
  if (is_rtsl) {
    spdlog::info("[ALLOC] Loading RTSL source: {}", opts.input_file);
    ptpn::Diagnostics local;
    ptpn::Diagnostics& collector = diagnostics != nullptr ? *diagnostics : local;
    std::optional<RtslLoadResult> loaded =
        load_tdg_from_rtsl(opts.input_file, dsl::ValidationOptions::Mode::Allocate, collector);
    if (!loaded.has_value()) {
      if (diagnostics == nullptr) {
        print_legacy_diagnostics(collector);
        return 1;
      }
      return collector.exit_code() != 0 ? collector.exit_code() : 1;
    }
    tdg = std::move(loaded->tdg);
  } else {
    spdlog::info("[ALLOC] Loading TDG JSON: {}", opts.input_file);
    parse::Parser parser;
    if (load_tdg_from_json(opts.input_file, tdg, parser, diagnostics) != 0) {
      return 1;
    }
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
    std::optional<detail::ScopedLogLevel> quiet;
    if (!opts.debug_mode) {
      quiet.emplace(spdlog::level::warn);
    }
    report = allocation::search_allocations(tdg, search_options);
  }

  if (diagnostics != nullptr) {
    diagnostics->set_result(nlohmann::json{{"allocation", allocation::report_to_json(report)}});
  }

  if (!allocation::save_report_json(report, opts.output_file)) {
    std::cerr << "ERROR: Failed to write allocation report: " << opts.output_file << std::endl;
    if (diagnostics != nullptr) {
      diagnostics->add("E-INT-002", "failed to write allocation report: " + opts.output_file,
                       "output:" + opts.output_file, std::nullopt,
                       "check that the output path is writable");
    }
    return 1;
  }

  spdlog::info("[ALLOC] Strategy: {} (complete={}), objective={}",
               report.strategy == allocation::Strategy::EXHAUSTIVE ? "exhaustive" : "heuristic",
               report.complete ? "true" : "false", allocation_objective_name(report.objective));
  spdlog::info("[ALLOC] Domain size: {}, evaluated: {}, feasible: {}{}", report.domain_size,
               report.evaluated, report.feasible_count,
               report.truncated ? " (search truncated)" : "");
  if (report.best.has_value()) {
    const allocation::CandidateSummary& best = report.best.value();
    spdlog::info("[ALLOC] Best assignment: {}", format_allocation_assignment(best.assignment));
    if (best.max_wcrt_infinite) {
      spdlog::info("[ALLOC] max_wcrt: infinite, max_core_load: {}", best.max_core_load);
    } else {
      spdlog::info("[ALLOC] max_wcrt: {}, max_core_load: {}", best.max_wcrt, best.max_core_load);
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
