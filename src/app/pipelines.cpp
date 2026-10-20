#include "app/pipelines.h"

#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "app/analysis.h"
#include "app/input.h"
#include "app/logging.h"
#include "common/diagnostics.h"
#include "export/tdg_dot.h"
#include "export/tdg_json_writer.h"
#include "export/wcet_json.h"
#include "model/tdg.h"
#include "parse/json.h"
#include "parse/ptpn_parser.h"

namespace app {

namespace {

// Shared `--export-wcet` failure handling for the TDG and RTSL paths.
int write_wcet_export(const tdg::TDG& tdg, const std::string& output_path,
                      ptpn::Diagnostics* diagnostics) {
  if (exporting::write_wcet_json(tdg, output_path)) {
    spdlog::info("[OUTPUT] WCET JSON exported to: {}", output_path);
    return 0;
  }
  spdlog::warn("[OUTPUT] Failed to save WCET JSON to: {}", output_path);
  if (diagnostics != nullptr) {
    diagnostics->add("E-INT-002", "failed to write WCET JSON: " + output_path,
                     "output:" + output_path, std::nullopt,
                     "check that the output path is writable");
  }
  return 1;
}

}  // namespace

int run_tdg_pipeline(const std::string& input_file, PipelineOptions opts, size_t initial_memory,
                     ptpn::Diagnostics* diagnostics) {
  const auto pipeline_start = detail::PipelineClock::now();
  const auto tdg_start = detail::PipelineClock::now();
  spdlog::info("[TDG] Loading JSON: {}", input_file);

  parse::Parser parser;
  tdg::TDG tdg(1, 1);
  if (load_tdg_from_json(input_file, tdg, parser, diagnostics) != 0) {
    return 1;
  }

  apply_policy_override(tdg, opts.policy_override);

  spdlog::info("[TDG] Configuration: {} CPUs, {} cores per CPU, policy={}", parser.get_num_cpus(),
               parser.get_cores_per_cpu(), schedule_policy_to_string(tdg.policy));

  if (!opts.exports.tdg_dot.empty()) {
    if (!tdg::export_to_dot(tdg, opts.exports.tdg_dot)) {
      spdlog::warn("[OUTPUT] Failed to export TDG DOT to: {}", opts.exports.tdg_dot);
      if (diagnostics != nullptr) {
        diagnostics->add("E-INT-002", "failed to write TDG DOT: " + opts.exports.tdg_dot,
                         "output:" + opts.exports.tdg_dot, std::nullopt,
                         "check that the output path is writable");
      }
      return 1;
    }
    spdlog::info("[OUTPUT] TDG DOT exported to: {}", opts.exports.tdg_dot);
  }

  if (!opts.exports.wcet_json.empty() &&
      write_wcet_export(tdg, opts.exports.wcet_json, diagnostics) != 0) {
    return 1;
  }

  const auto tdg_end = detail::PipelineClock::now();
  const size_t tdg_memory = get_memory_usage() - initial_memory;
  detail::log_step_timing("TDG parsing", detail::elapsed_ms(tdg_start),
                          std::to_string(tdg_memory) + " KB");

  return run_tdg_analysis(tdg, opts, "TDG", pipeline_start, initial_memory, diagnostics);
}

int run_rtsl_pipeline(const std::string& input_file, PipelineOptions opts, size_t initial_memory,
                      ptpn::Diagnostics* diagnostics) {
  const auto pipeline_start = detail::PipelineClock::now();
  spdlog::info("[RTSL] Loading source: {}", input_file);

  ptpn::Diagnostics local;
  ptpn::Diagnostics& collector = diagnostics != nullptr ? *diagnostics : local;
  std::optional<RtslLoadResult> loaded =
      load_tdg_from_rtsl(input_file, dsl::ValidationOptions::Mode::Rtsl, collector);
  if (!loaded.has_value()) {
    if (diagnostics == nullptr) {
      print_legacy_diagnostics(collector);
      return 1;
    }
    return collector.exit_code() != 0 ? collector.exit_code() : 1;
  }

  tdg::TDG& tdg = loaded->tdg;
  apply_policy_override(tdg, opts.policy_override);
  spdlog::info("[TDG] Configuration: {} CPUs, {} cores per CPU, policy={}", tdg.num_cpus,
               tdg.cores_per_cpu, schedule_policy_to_string(tdg.policy));

  if (!opts.exports.tdg_json.empty()) {
    if (!exporting::write_tdg_json(tdg, opts.exports.tdg_json)) {
      spdlog::warn("[OUTPUT] Failed to save TDG JSON to: {}", opts.exports.tdg_json);
      if (diagnostics != nullptr) {
        diagnostics->add("E-INT-002", "failed to write TDG JSON: " + opts.exports.tdg_json,
                         "output:" + opts.exports.tdg_json, std::nullopt,
                         "check that the output path is writable");
      }
      return 1;
    }
    spdlog::info("[OUTPUT] TDG JSON exported to: {}", opts.exports.tdg_json);
  }

  if (!opts.exports.wcet_json.empty() &&
      write_wcet_export(tdg, opts.exports.wcet_json, diagnostics) != 0) {
    return 1;
  }

  detail::log_step_timing("RTSL lowering", detail::elapsed_ms(pipeline_start),
                          std::to_string(get_memory_usage() - initial_memory) + " KB");

  return run_tdg_analysis(tdg, opts, "RTSL", pipeline_start, initial_memory, diagnostics,
                          &loaded->source_map);
}

int run_ptpn_pipeline(const std::string& input_file, const PipelineOptions& opts,
                      size_t initial_memory, ptpn::Diagnostics* diagnostics) {
  const auto pipeline_start = detail::PipelineClock::now();
  const auto parse_start = detail::PipelineClock::now();
  spdlog::info("[PTPN] Loading source: {}", input_file);

  const auto parse_result = diagnostics != nullptr
                                ? parser::PTPNBuilder::parse_file(input_file, *diagnostics)
                                : parser::PTPNBuilder::parse_file(input_file);
  if (!parse_result.ok()) {
    std::cerr << "ERROR: Failed to parse PTPN file: " << parse_result.error() << std::endl;
    return 1;
  }
  const petri::PTPN& ptpn = parse_result.value();

  detail::log_step_timing("PTPN parsing", detail::elapsed_ms(parse_start));

  spdlog::info("[PTPN] Parse completed");
  spdlog::info("  Places: {}", ptpn.num_places());
  spdlog::info("  Transitions: {}", ptpn.num_transitions());

  if (!should_run_analysis(opts) && !has_any_export(opts.exports)) {
    spdlog::warn("[MAIN] No exports requested and analysis disabled; nothing to do");
    return 0;
  }

  return run_ptpn_postprocess(ptpn, "PTPN", opts, pipeline_start, initial_memory, diagnostics);
}

}  // namespace app
