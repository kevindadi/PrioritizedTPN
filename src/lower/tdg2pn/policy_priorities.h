#ifndef LOWER_TDG2PN_POLICY_PRIORITIES_H
#define LOWER_TDG2PN_POLICY_PRIORITIES_H

#include <string>
#include <unordered_map>

#include "model/tdg.h"

namespace converter::detail {

// Computes the static priorities encoded on transitions when a policy derives
// them from task timing metadata.
//
//   RM: key = periodic period (positive only); shorter period wins.
//   DM: key = explicit deadline when > 0, otherwise the periodic period;
//       shorter key wins. Tasks with neither get the lowest priority.
//
// Equal keys share the same integer priority. Unique key groups are ranked so
// that the highest-priority group gets the largest integer and the lowest group
// gets 1; tasks without a key get 0. Ties are grouped by key; task names are
// used only to keep the rank assignment deterministic. Explicit `priority`
// values in the input are ignored for RM/DM.
//
// For every other SchedulePolicy this is a no-op: the returned map contains the
// tasks' current priorities.
std::unordered_map<std::string, int> compute_policy_priorities(const tdg::TDG& tdg);

// Applies compute_policy_priorities() to TaskNode.priority in both
// tdg.all_task and tdg.nodes_type, and to tdg.tasks_priority. No-op for
// policies other than RM/DM.
void apply_policy_priorities(tdg::TDG& tdg);

}  // namespace converter::detail

#endif  // LOWER_TDG2PN_POLICY_PRIORITIES_H
