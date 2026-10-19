#ifndef ANALYSIS_REACHABILITY_SCHEDULING_POLICY_COMMON_H
#define ANALYSIS_REACHABILITY_SCHEDULING_POLICY_COMMON_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "analysis/reachability/state_class.h"
#include "model/petri.h"

namespace state_class {

// Least fixpoint of eff[holder] = max(eff[holder], eff[waiter]) over the wait
// edges. Shared by PIP, PCP and SRP inheritance.
void inherit_priorities(std::vector<int>& effective,
                        const std::vector<std::pair<int, int>>& edges, size_t task_count);

std::string task_name_or_ordinal(const std::vector<TaskChain>& tasks, size_t ordinal);

// Task-chain index shared by the per-job-clock policies (EDF, FIFO, LLF-ED).
struct ReleaseClassOptions {
  const char* label = "EDF";
  bool require_deadline = true;
  bool use_cpu_places = false;
  bool require_deterministic_segments = false;
  bool require_single_core = false;
};

struct ReleaseTaskIndex {
  std::vector<TaskChain> tasks;
  std::vector<int> task_of_transition;
  std::vector<int> task_of_periodic_transition;
};

ReleaseTaskIndex build_release_task_index(const petri::PTPN& net,
                                          const ReleaseClassOptions& options);

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_POLICY_COMMON_H
