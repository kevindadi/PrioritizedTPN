#ifndef ANALYSIS_DBM_DBM_INTERNAL_H
#define ANALYSIS_DBM_DBM_INTERNAL_H

#include <limits>

#include "analysis/dbm/clock_state.h"

namespace state_class::detail {

// Saturating addition of two DBM bounds; any overflow collapses to +inf (or
// INT_MIN for a negative overflow), matching the symbolic bound semantics.
inline int safe_add_bound(int lhs, int rhs) {
  if (lhs == INF_TIME || rhs == INF_TIME) {
    return INF_TIME;
  }

  if ((rhs > 0 && lhs > std::numeric_limits<int>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<int>::min() - rhs)) {
    return rhs > 0 ? INF_TIME : std::numeric_limits<int>::min();
  }

  return lhs + rhs;
}

}  // namespace state_class::detail

#endif  // ANALYSIS_DBM_DBM_INTERNAL_H
