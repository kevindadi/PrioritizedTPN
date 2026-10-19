#include "app/check_cmd.h"

#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/rtsl_input.h"
#include "common/diagnostics.h"
#include "dsl/validate.h"

namespace app {

int run_rtsl_check(const CheckCommandOptions& opts, ptpn::Diagnostics* diagnostics) {
  ptpn::Diagnostics local;
  ptpn::Diagnostics& collector = diagnostics != nullptr ? *diagnostics : local;

  std::optional<std::string> source = read_rtsl_source(opts.input_file);
  if (!source.has_value()) {
    if (diagnostics != nullptr) {
      add_rtsl_open_error(opts.input_file, *diagnostics);
      return diagnostics->exit_code();
    }
    // Legacy check has always printed this without a diagnostic code.
    std::cerr << "ERROR: failed to open RTSL file: " << opts.input_file << '\n';
    return 1;
  }

  RtslDocument document = parse_and_validate_rtsl(
      std::move(source.value()), opts.input_file, dsl::ValidationOptions::Mode::Check, collector);

  const bool ok = collector.ok();
  if (ok) {
    collector.set_result(nlohmann::json{
        {"pending_measurements",
         dsl::collect_pending_measurements(document.program, document.source_map)}});
  }

  if (diagnostics == nullptr) {
    print_legacy_diagnostics(collector);
  }

  std::cerr << "RTSL check: " << document.program.tasks.size() << " tasks, "
            << document.program.edges.size() << " edges, "
            << (ok ? std::string("ok") : std::string("failed")) << '\n';

  if (diagnostics == nullptr) {
    return ok ? 0 : 1;
  }
  return collector.exit_code();
}

}  // namespace app
