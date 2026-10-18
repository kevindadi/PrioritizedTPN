#ifndef ANALYSIS_REDUCTIONS_CONE_PROJECTION_H
#define ANALYSIS_REDUCTIONS_CONE_PROJECTION_H

#include <string>

#include "model/petri.h"

namespace petri {

// Builds the per-task cone of `net`: the smallest prefix-closed sub-net that can
// influence the response time of `task`.
//
// The closure starts from the task's chain and repeatedly adds
//
//   1. the task chain of every task on the same real core (CPU competition);
//   2. every task holding a lock used anywhere in the cone, plus its lock place
//      (transitive shared-lock closure);
//   3. dependency predecessors and successors, the bridge/fork/join/control
//      transitions on the paths between them, and periodic-release transitions
//      with their period places;
//
// until a fixed point is reached. The projected net preserves arcs between
// retained nodes, the initial marking, and the lowering metadata needed by the
// analysis layer (task_info, node_pn_map, node_start_end_map, lock_places,
// periodic_transitions, cpu_places and core parallelism) for retained tasks.
//
// Throws std::invalid_argument when `task` has no lowering metadata in `net`
// (for example a hand-written .ptpn model, which has no task_info).
PTPN project_cone(const PTPN& net, const std::string& task);

}  // namespace petri

#endif  // ANALYSIS_REDUCTIONS_CONE_PROJECTION_H
