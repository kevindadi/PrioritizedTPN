#include "app/tdg_input.h"

#include <iostream>
#include <utility>

#include <spdlog/spdlog.h>

#include "common/diagnostics.h"
#include "model/tdg.h"
#include "parse/json.h"

namespace app {

void apply_policy_override(tdg::TDG& tdg, const std::optional<SchedulePolicy>& policy_override) {
  if (!policy_override.has_value()) {
    return;
  }
  tdg.policy = policy_override.value();
  spdlog::info("[TDG] Scheduling policy override: {}", schedule_policy_to_string(tdg.policy));
}

int load_tdg_from_json(const std::string& input_file, tdg::TDG& tdg, parse::Parser& parser,
                       ptpn::Diagnostics* diagnostics) {
  const auto parse_result = parser.parse_file(input_file);
  if (!parse_result.success) {
    std::cerr << "ERROR: Failed to parse JSON file: " << parse_result.error_message << std::endl;
    if (diagnostics != nullptr) {
      diagnostics->append(parser.diagnostics());
    }
    return 1;
  }

  const auto validation = parser.validate();
  if (diagnostics != nullptr) {
    diagnostics->append(parser.diagnostics());
  }
  if (!validation.success) {
    std::cerr << "ERROR: Input validation failed:" << std::endl;
    for (const auto& err : validation.errors) {
      std::cerr << "  - " << err << std::endl;
    }
    return 1;
  }

  if (!validation.warnings.empty()) {
    std::cout << "Warnings:" << std::endl;
    for (const auto& warn : validation.warnings) {
      std::cout << "  - " << warn << std::endl;
    }
  }

  tdg = tdg::TDG(parser.get_num_cpus(), parser.get_cores_per_cpu());
  tdg.load_from_parser(parser, /*log_nodes=*/true);
  return 0;
}

}  // namespace app
