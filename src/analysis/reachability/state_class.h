#ifndef ANALYSIS_REACHABILITY_STATE_CLASS_H
#define ANALYSIS_REACHABILITY_STATE_CLASS_H

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "analysis/dbm/dbm.h"

namespace state_class {

constexpr size_t NO_TRANSITION = std::numeric_limits<size_t>::max();

// Sorted vector of transition ids used as a lightweight ordered set: far more
// compact and copy-friendly than std::set for the small, read-mostly enabling
// sets carried by every state class.
using TransitionSet = std::vector<size_t>;

inline bool contains(const TransitionSet& set, size_t value) {
  return std::binary_search(set.begin(), set.end(), value);
}

// Every column/row of the joint DBM corresponds to one timed variable.
enum class ClockKind {
  Zero,        // the reference variable x0 (always lives at index 0)
  Execution,   // h_t: how long transition t has effectively been running
  Suspension,  // w_t: how long suspendable transition t has been suspended
};

// Maps a DBM column index back to the variable it represents.
struct ClockVar {
  ClockKind kind = ClockKind::Zero;
  size_t transition = NO_TRANSITION;

  bool operator==(const ClockVar& other) const {
    return kind == other.kind && transition == other.transition;
  }

  bool operator<(const ClockVar& other) const {
    if (kind != other.kind)
      return kind < other.kind;
    return transition < other.transition;
  }
};

// One edge of the state-class graph: which transition fired, plus the feasible
// firing window of its execution clock h_t at the moment it fires. The window
// [firing_min, firing_max] is the real (symbolic) timing; firing_max may be
// +infinity (INF_TIME).
//
// [dwell_min, dwell_max] is the global time the net may spend in the SOURCE
// state class before this firing (the amount every active clock advances). It is
// the per-edge time weight all timing metrics build on; dwell_max may be
// +infinity (INF_TIME).
struct FiringEdge {
  int transition_id = -1;
  int firing_min = 0;
  int firing_max = 0;
  int dwell_min = 0;
  int dwell_max = 0;

  FiringEdge() = default;

  FiringEdge(int id, int window_min, int window_max)
      : transition_id(id), firing_min(window_min), firing_max(window_max) {}

  FiringEdge(int id, int window_min, int window_max, int stay_min, int stay_max)
      : transition_id(id),
        firing_min(window_min),
        firing_max(window_max),
        dwell_min(stay_min),
        dwell_max(stay_max) {}

  bool operator==(const FiringEdge& other) const {
    return transition_id == other.transition_id && firing_min == other.firing_min &&
           firing_max == other.firing_max && dwell_min == other.dwell_min &&
           dwell_max == other.dwell_max;
  }

  [[nodiscard]] std::string to_string() const;
};

// Symbolic state class C = (M, Omega) together with the scheduler-facing sets
// that the priority semantics needs. The DBM `zone` ranges over the dynamic
// variable vector X = {x0} U {h_t : t in struct_enabled}
//                       U {w_t : t in suspended}.
struct StateClass {
  std::vector<int> marking;  // M
  DBM zone;                  // Omega

  std::vector<ClockVar> clock_vars;           // index -> variable; [0] is Zero
  std::vector<int> exec_clock_of_transition;  // transition -> h_t index or -1
  std::vector<int> susp_clock_of_transition;  // transition -> w_t index or -1

  TransitionSet struct_enabled;    // E_struct(M)
  TransitionSet priority_enabled;  // E_pri(M): active transitions
  TransitionSet suspended;         // (E_struct \ E_pri) intersect T2

  double elapsed_time = 0.0;  // auxiliary metadata, excluded from identity
  size_t id = 0;              // assigned when inserted into the graph

  [[nodiscard]] int exec_index(size_t transition) const {
    return transition < exec_clock_of_transition.size() ? exec_clock_of_transition[transition] : -1;
  }

  [[nodiscard]] int susp_index(size_t transition) const {
    return transition < susp_clock_of_transition.size() ? susp_clock_of_transition[transition] : -1;
  }

  [[nodiscard]] bool has_exec_clock(size_t transition) const {
    return exec_index(transition) > 0;
  }

  [[nodiscard]] bool has_susp_clock(size_t transition) const {
    return susp_index(transition) > 0;
  }
};

// Hashes the marking alone so candidates that might merge land in one bucket.
struct MarkingHash {
  size_t operator()(const std::vector<int>& marking) const;
};

// Exact-identity hash over (marking, clock_vars, zone matrix) used for O(1)
// deduplication in equality mode. Collisions must be resolved with
// check_equality() against the stored state.
size_t hash_state_class(const StateClass& state);

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_STATE_CLASS_H
