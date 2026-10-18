#ifndef ANALYSIS_DBM_CLOCK_STATE_H
#define ANALYSIS_DBM_CLOCK_STATE_H

#include <limits>

namespace state_class {

// Sentinel used inside DBM matrices for "no upper bound" (+infinity).
constexpr int INF_TIME = std::numeric_limits<int>::max();

}  // namespace state_class

#endif  // ANALYSIS_DBM_CLOCK_STATE_H
