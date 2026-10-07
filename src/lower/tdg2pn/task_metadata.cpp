#include "lower/tdg2pn/task_metadata.h"

#include <string>
#include <unordered_map>

namespace converter::detail {

void populate_task_info(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  // Period comes from the explicit periodic bindings; tasks without one are
  // aperiodic.
  std::unordered_map<std::string, int> period_of;
  for (const auto& binding : tdg.periodic_tasks) {
    period_of[binding.task] = binding.period;
  }

  for (const auto& node : tdg.all_task) {
    const auto* task = as_task_node(node);
    if (task == nullptr) {
      continue;
    }
    petri::TaskInfo info;
    info.core = task->core;
    info.priority = task->priority;
    info.wcet = 0;
    info.bcet = 0;
    for (const auto& segment : task->time) {
      info.bcet += segment.first;
      info.wcet += segment.second;
    }
    const auto period_it = period_of.find(task->name);
    info.period = period_it == period_of.end() ? 0 : period_it->second;
    info.deadline = info.period;  // implicit deadline = period
    const auto locks_it = tdg.task_locks_map.find(task->name);
    if (locks_it != tdg.task_locks_map.end()) {
      info.locks = locks_it->second;
    } else {
      info.locks = task->lock;
    }
    ptpn.task_info[task->name] = std::move(info);
  }
}

}  // namespace converter::detail
