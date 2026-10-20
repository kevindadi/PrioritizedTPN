#ifndef ANALYSIS_METRICS_METRICS_INTERNAL_H
#define ANALYSIS_METRICS_METRICS_INTERNAL_H

#include <cstddef>
#include <vector>

#include "analysis/dbm/clock_state.h"

namespace state_class {

// Shared by the metrics sub-analyses: true for the symbolic +infinity bound.
inline bool is_inf(int value) {
  return value == INF_TIME;
}

// Iterative Tarjan SCC over a masked subgraph. Returns the component id of
// every vertex (-1 when the vertex is outside the subgraph). Iterative so that
// deep state-class graphs (hundreds of thousands of vertices) cannot overflow
// the call stack. Shared by the local-deadlock and steady-cycle analyses.
std::vector<long long> tarjan_components(const std::vector<std::vector<size_t>>& adjacency,
                                         const std::vector<char>& in_subgraph);

}  // namespace state_class

#endif  // ANALYSIS_METRICS_METRICS_INTERNAL_H
