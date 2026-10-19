#ifndef APP_ALLOCATION_CMD_H
#define APP_ALLOCATION_CMD_H

#include <cstddef>
#include <optional>
#include <string>

#include "analysis/allocation/allocation.h"
#include "app/common.h"
#include "model/types.h"

namespace ptpn {
class Diagnostics;
}

namespace app {

struct AllocationCommandOptions {
  std::string input_file;
  std::string output_file;
  size_t max_candidates = 64;
  size_t candidate_max_states = 200000;
  allocation::Objective objective = allocation::Objective::MAX_WCRT;
  bool require_certified_wcet = false;
  bool debug_mode = false;
  std::optional<SchedulePolicy> policy_override;
  DiagnosticsOptions diagnostics;
};

int run_allocation(const AllocationCommandOptions& opts, ptpn::Diagnostics* diagnostics = nullptr);

}  // namespace app

#endif  // APP_ALLOCATION_CMD_H
