#ifndef ANALYSIS_METRICS_INTERNAL_H
#define ANALYSIS_METRICS_INTERNAL_H

#include "analysis/clock_state.h"

namespace state_class {

// Shared by the metrics sub-analyses: true for the symbolic +infinity bound.
inline bool is_inf(int value) {
  return value == INF_TIME;
}

}  // namespace state_class

#endif  // ANALYSIS_METRICS_INTERNAL_H
