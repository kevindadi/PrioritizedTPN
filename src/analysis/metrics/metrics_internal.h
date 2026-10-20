#ifndef ANALYSIS_METRICS_METRICS_INTERNAL_H
#define ANALYSIS_METRICS_METRICS_INTERNAL_H

#include <cstddef>
#include <vector>

#include "analysis/dbm/clock_state.h"
#include "analysis/metrics/metrics.h"

namespace state_class {

// Shared by the metrics sub-analyses: true for the symbolic +infinity bound.
inline bool is_inf(int value) {
  return value == INF_TIME;
}

// Three-valued deadline verdict from the certified bounds. `graph_complete` is
// false when the state space was cut (max-states or horizon), which makes
// "meets" and starvation conclusions uncertifiable; `wcrt_lb_valid` is false
// under zone-merging reductions, which makes lower bounds uncertified.
// Callers that know an unbounded response is proven (a real cycle, a reached
// cap, or a complete graph with no completion edge) can pass `infinite_proven`
// to certify the miss even when the bounds alone would not.
inline DeadlineVerdict deadline_verdict_from_bounds(const TaskMetrics& task, bool graph_complete,
                                                    bool wcrt_lb_valid,
                                                    bool infinite_proven = false) {
  if (!task.has_deadline) {
    return DeadlineVerdict::Unknown;
  }
  if (infinite_proven) {
    return DeadlineVerdict::Misses;
  }
  if (wcrt_lb_valid && !task.wcrt_lb.infinite && task.wcrt_lb.value > task.deadline) {
    return DeadlineVerdict::Misses;
  }
  if (task.wcrt.infinite) {
    // An unbounded upper bound is a miss only when the graph is complete; with
    // a cut the missing part may contain the completion.
    return graph_complete ? DeadlineVerdict::Misses : DeadlineVerdict::Unknown;
  }
  if (graph_complete && task.wcrt.value <= task.deadline) {
    return DeadlineVerdict::Meets;
  }
  return DeadlineVerdict::Unknown;
}

// Iterative Tarjan SCC over a masked subgraph. Returns the component id of
// every vertex (-1 when the vertex is outside the subgraph). Iterative so that
// deep state-class graphs (hundreds of thousands of vertices) cannot overflow
// the call stack. Shared by the local-deadlock and steady-cycle analyses.
std::vector<long long> tarjan_components(const std::vector<std::vector<size_t>>& adjacency,
                                         const std::vector<char>& in_subgraph);

}  // namespace state_class

#endif  // ANALYSIS_METRICS_METRICS_INTERNAL_H
