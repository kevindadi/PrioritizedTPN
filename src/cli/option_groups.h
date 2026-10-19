#ifndef CLI_OPTION_GROUPS_H
#define CLI_OPTION_GROUPS_H

#include <CLI/CLI.hpp>
#include <string>

#include "app/pipeline.h"
#include "model/types.h"

namespace cli {

// `--diagnostics json` and `--diagnostics-file PATH`: selects the versioned
// diagnostics envelope transport.
void add_diagnostics_options(CLI::App* cmd, app::DiagnosticsOptions& diagnostics);

// `--export-tdg`, `--export-wcet`, `--export-ptpn`, `--export-scg` and
// `--export-metrics`. TDG/PTPN materialize a Graphviz DOT file for
// `--export-tdg`, while the RTSL path materializes the lowered TDG JSON.
void add_export_target_options(CLI::App* cmd, app::ExportTargets& exports,
                               bool tdg_export_is_json = false);

// Analysis options shared by `tdg`, `rtsl` and `ptpn`: state-space bounds,
// canonicalization/reduction modes, extrapolation, suspension clocks and the
// WCRT time horizon, plus the diagnostics and export groups.
void add_common_pipeline_options(CLI::App* cmd, app::PipelineOptions& opts,
                                 bool tdg_export_is_json = false);

// Parses a `--policy` value, throwing CLI::ValidationError for unknown names.
SchedulePolicy parse_policy_option(const std::string& policy);

}  // namespace cli

#endif  // CLI_OPTION_GROUPS_H
