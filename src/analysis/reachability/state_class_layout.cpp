#include <algorithm>

#include "analysis/dbm/clock_state.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"

namespace state_class {

int StateClassReachabilityGraph::effective_earliest(size_t transition) const {
  return net_.get_transition(petri::TransitionId{transition}).time_interval.effective_earliest();
}

int StateClassReachabilityGraph::effective_latest(size_t transition) const {
  const int latest =
      net_.get_transition(petri::TransitionId{transition}).time_interval.effective_latest();
  return latest == petri::INF ? INF_TIME : latest;
}

void StateClassReachabilityGraph::recompute_sets(StateClass& state) const {
  state.struct_enabled = Scheduling::structural_enabled(net_, state.marking);
  state.priority_enabled = Scheduling::filter_priority_per_core(state.struct_enabled, net_);

  state.suspended.clear();
  // struct_enabled is sorted, so suspended stays sorted too.
  for (size_t t : state.struct_enabled) {
    if (contains(state.priority_enabled, t)) {
      continue;
    }
    if (net_.get_transition(petri::TransitionId{t}).suspendable) {
      state.suspended.push_back(t);
    }
  }
}

void StateClassReachabilityGraph::build_layout(StateClass& state) const {
  const size_t num_transitions = net_.num_transitions();

  state.clock_vars.clear();
  state.clock_vars.push_back({ClockKind::Zero, NO_TRANSITION});
  state.exec_clock_of_transition.assign(num_transitions, -1);
  state.susp_clock_of_transition.assign(num_transitions, -1);

  for (size_t t : state.struct_enabled) {
    const int exec_idx = static_cast<int>(state.clock_vars.size());
    state.clock_vars.push_back({ClockKind::Execution, t});
    state.exec_clock_of_transition[t] = exec_idx;

    if (contains(state.suspended, t)) {
      const int susp_idx = static_cast<int>(state.clock_vars.size());
      state.clock_vars.push_back({ClockKind::Suspension, t});
      state.susp_clock_of_transition[t] = susp_idx;
    }
  }
}

StateClass StateClassReachabilityGraph::compute_initial_class() {
  StateClass state;
  state.marking = net_.get_marking();
  state.elapsed_time = 0.0;

  recompute_sets(state);
  build_layout(state);

  // Every clock starts at zero, i.e. pinned to the reference variable x0.
  const size_t n = state.clock_vars.size();
  DBM zone(n);
  for (size_t i = 1; i < n; ++i) {
    zone.set_constraint(0, i, 0);
    zone.set_constraint(i, 0, 0);
  }
  zone.minimize();
  state.zone = std::move(zone);

  return state;
}

}  // namespace state_class
