#include <algorithm>

#include "analysis/dbm/clock_state.h"
#include "analysis/reachability/ptpn_analysis.h"

namespace state_class {

StateClass StateClassReachabilityGraph::time_elapse(const StateClass& state) const {
  StateClass out = state;
  const size_t n = out.zone.size();
  if (n == 0) {
    return out;
  }

  // Running clocks advance at rate 1; everything else (x0 and the frozen
  // execution clocks of priority-filtered-out transitions) stays put.
  // V_run = { h_t : t in E_pri } U { w_t : t in suspended }.
  std::vector<bool> running(n, false);
  for (size_t i = 1; i < n && i < out.clock_vars.size(); ++i) {
    const ClockVar& var = out.clock_vars[i];
    if (var.kind == ClockKind::Suspension) {
      running[i] = true;  // suspension clocks always advance
    } else if (var.kind == ClockKind::Execution && contains(out.priority_enabled, var.transition)) {
      running[i] = true;  // active execution clocks advance
    }
  }

  // Release each running clock's upper bound relative to every stationary
  // variable (x0 and the frozen clocks). The differences between two running
  // clocks are left untouched, so they keep growing synchronously.
  for (size_t i = 1; i < n; ++i) {
    if (!running[i]) {
      continue;
    }
    for (size_t j = 0; j < n; ++j) {
      if (j != i && !running[j]) {
        out.zone.set_constraint(i, j, INF_TIME);
      }
    }
  }

  // Strong time semantics: cap each active execution clock at its deadline
  // upSI(t) so time cannot advance past a transition that must fire.
  for (size_t t : out.priority_enabled) {
    if (!out.has_exec_clock(t)) {
      continue;
    }
    const int upper = effective_latest(t);
    if (upper == INF_TIME) {
      continue;
    }
    const size_t idx = static_cast<size_t>(out.exec_index(t));
    const int current = out.zone.get_constraint(idx, 0);
    if (current == INF_TIME || upper < current) {
      out.zone.set_constraint(idx, 0, upper);
    }
  }

  out.zone.minimize();
  return out;
}

bool StateClassReachabilityGraph::is_firable(const StateClass& elapsed, size_t t) const {
  if (!contains(elapsed.priority_enabled, t) || !elapsed.has_exec_clock(t)) {
    return false;
  }

  const size_t idx = static_cast<size_t>(elapsed.exec_index(t));
  const int max_h = elapsed.zone.get_constraint(idx, 0);  // largest feasible h_t
  if (max_h == INF_TIME) {
    return true;
  }
  return max_h >= effective_earliest(t);
}

void StateClassReachabilityGraph::build_successor_zone(StateClass& successor, const DBM& fired,
                                                       const StateClass& source,
                                                       size_t fired_transition) const {
  const size_t n = successor.clock_vars.size();
  DBM zone(n);

  // For each successor variable, find the matching column in `fired` if the
  // clock survives the firing (a persistent execution clock keeps its value; a
  // still-suspended clock inherits its suspension clock). Everything else is
  // freshly created and starts at zero.
  std::vector<int> source_index(n, -1);
  source_index[0] = 0;  // x0 maps to x0
  for (size_t i = 1; i < n; ++i) {
    const ClockVar& var = successor.clock_vars[i];
    const size_t t = var.transition;
    if (var.kind == ClockKind::Execution) {
      if (t != fired_transition && source.has_exec_clock(t)) {
        source_index[i] = source.exec_index(t);
      }
    } else {  // Suspension
      if (t != fired_transition && source.has_susp_clock(t)) {
        source_index[i] = source.susp_index(t);
      }
    }
  }

  // Carry over the joint constraints between all surviving clocks.
  for (size_t i = 0; i < n; ++i) {
    if (source_index[i] < 0) {
      continue;
    }
    for (size_t j = 0; j < n; ++j) {
      if (source_index[j] < 0) {
        continue;
      }
      zone.set_constraint(i, j,
                          fired.get_constraint(static_cast<size_t>(source_index[i]),
                                               static_cast<size_t>(source_index[j])));
    }
  }

  // Pin every freshly created clock to zero (equal to x0).
  for (size_t i = 1; i < n; ++i) {
    if (source_index[i] < 0) {
      zone.set_constraint(0, i, 0);
      zone.set_constraint(i, 0, 0);
    }
  }

  zone.minimize();
  successor.zone = std::move(zone);
}

bool StateClassReachabilityGraph::fire(const StateClass& elapsed, size_t t,
                                       StateClass& successor) const {
  if (!is_firable(elapsed, t)) {
    return false;
  }

  // Step 1: intersect the firing-domain constraint h_t >= downSI(t). The
  // elapsed zone is already canonical, so a single-constraint incremental
  // tightening (O(n^2)) replaces the full closure.
  DBM fired = elapsed.zone;
  const size_t idx = static_cast<size_t>(elapsed.exec_index(t));
  const int lower = effective_earliest(t);
  if (!fired.tighten(0, idx, -lower)) {
    return false;
  }

  // Step 2: discrete token shuffle.
  successor = StateClass();
  successor.marking = petri::PTPN::fire(elapsed.marking, net_, petri::TransitionId{t});

  // Steps 3 & 4: recompute the scheduler sets and the variable layout, then
  // rebuild the zone carrying surviving clocks over from `fired`.
  recompute_sets(successor);
  build_layout(successor);
  build_successor_zone(successor, fired, elapsed, t);

  const int h_lower = -elapsed.zone.get_constraint(0, idx);
  const int firing_instant = std::max(lower, h_lower);
  successor.elapsed_time = elapsed.elapsed_time + std::max(0, firing_instant);

  return true;
}

}  // namespace state_class
