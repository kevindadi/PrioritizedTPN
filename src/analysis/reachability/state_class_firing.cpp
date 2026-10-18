#include <algorithm>
#include <stdexcept>

#include "analysis/dbm/clock_state.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"

namespace state_class {

namespace {

// Intersects the EDF deadline-order constraints with the successor zone.
// Returns false when the branch is infeasible and must be dropped.
bool apply_branch_constraints(StateClass& state,
                              const std::vector<BranchConstraint>& constraints) {
  for (const BranchConstraint& constraint : constraints) {
    const int left = state.release_index(constraint.left_task);
    const int right = state.release_index(constraint.right_task);
    if (left <= 0 || right <= 0) {
      throw std::invalid_argument(
          "release-clock constraint references a task without an in-flight job");
    }
    // release[right] - release[left] <= bound.
    if (!state.zone.tighten(static_cast<size_t>(right), static_cast<size_t>(left),
                            constraint.bound)) {
      return false;
    }
  }
  return true;
}

}  // namespace

StateClass StateClassReachabilityGraph::time_elapse(const StateClass& state) const {
  StateClass out = state;
  const size_t n = out.zone.size();
  if (n == 0) {
    return out;
  }

  // Running clocks advance at rate 1; everything else (x0 and the frozen
  // execution clocks of priority-filtered-out transitions) stays put.
  // V_run = { r_i : i in flight } U { h_t : t in E_pri }
  //         U { w_t : t in suspended }.
  std::vector<bool> running(n, false);
  for (size_t i = 1; i < n && i < out.clock_vars.size(); ++i) {
    const ClockVar& var = out.clock_vars[i];
    if (var.kind == ClockKind::Suspension) {
      // Only present when suspension-clock projection is disabled; such clocks
      // always advance with time.
      running[i] = true;
    } else if (var.kind == ClockKind::Release) {
      // A release clock exists exactly while its job is in flight and measures
      // wall-clock time since release; it always runs, even when the job is
      // preempted. This is what keeps the remaining deadline D_i - r_i
      // comparable across a preemption.
      running[i] = true;
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
  // still-in-flight release clock keeps the job age; a still-suspended clock
  // inherits its suspension clock). Everything else is freshly created and
  // starts at zero.
  std::vector<int> source_index(n, -1);
  source_index[0] = 0;  // x0 maps to x0
  for (size_t i = 1; i < n; ++i) {
    const ClockVar& var = successor.clock_vars[i];
    const size_t t = var.transition;
    if (var.kind == ClockKind::Release) {
      // Release clocks are keyed by task ordinal, not by transition. A task
      // that is in flight in both source and successor keeps its clock (also
      // when a periodic release merges into the running job). A task that was
      // not in flight has no source clock and the fresh clock is pinned to
      // zero, which is exactly the release instant. Completed jobs have no
      // successor release clock, so their column is simply dropped.
      source_index[i] = source.release_index(var.task);
    } else if (var.kind == ClockKind::Execution) {
      if (t != fired_transition && source.has_exec_clock(t)) {
        source_index[i] = source.exec_index(t);
      }
    } else {  // Suspension (only present when projection is disabled)
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

std::vector<StateClass> StateClassReachabilityGraph::fire_all(const StateClass& elapsed,
                                                              size_t t) const {
  std::vector<StateClass> result;
  if (!is_firable(elapsed, t)) {
    return result;
  }

  // Step 1: intersect the firing-domain constraint h_t >= downSI(t). The
  // elapsed zone is already canonical, so a single-constraint incremental
  // tightening (O(n^2)) replaces the full closure.
  DBM fired = elapsed.zone;
  const size_t idx = static_cast<size_t>(elapsed.exec_index(t));
  const int lower = effective_earliest(t);
  if (!fired.tighten(0, idx, -lower)) {
    return result;
  }

  // Step 2: discrete token shuffle (branch-independent).
  const petri::Marking marking =
      petri::PTPN::fire(elapsed.marking, net_, petri::TransitionId{t});
  const TransitionSet struct_enabled = Scheduling::structural_enabled(net_, marking);

  // Rules that read release clocks need a prepared successor state: the
  // release-clock prefix depends only on the successor marking, so a probe
  // built with the full structural set has the same release indices as every
  // final branch. Its zone is the unsplit successor zone (surviving clocks
  // carried over, fresh clocks zero); EDF tests each candidate ordering
  // against it and emits the selected difference constraints.
  StateClass prepared;
  prepared.marking = marking;
  if (firing_rule_->uses_release_clocks()) {
    recompute_sets(prepared, struct_enabled);
    build_layout(prepared);
    build_successor_zone(prepared, fired, elapsed, t);
  }
  const PolicyContext context{net_, &prepared};
  const std::vector<PolicyBranch> branches =
      firing_rule_->branches(struct_enabled, net_, context);

  const int h_lower = -elapsed.zone.get_constraint(0, idx);
  const int firing_instant = std::max(lower, h_lower);

  result.reserve(branches.size());
  for (const PolicyBranch& branch : branches) {
    // Steps 3 & 4: scheduler sets and variable layout for this target branch,
    // then rebuild the zone carrying surviving clocks over from `fired`.
    StateClass successor;
    successor.marking = marking;
    recompute_sets(successor, branch.active);
    build_layout(successor);
    build_successor_zone(successor, fired, elapsed, t);
    // Deadline-order constraints are part of the successor zone before it is
    // hashed/deduplicated. A branch whose ordering is impossible after the
    // firing is not a behavior of the net.
    if (!apply_branch_constraints(successor, branch.constraints)) {
      continue;
    }
    successor.elapsed_time = elapsed.elapsed_time + std::max(0, firing_instant);
    result.push_back(std::move(successor));
  }

  return result;
}

bool StateClassReachabilityGraph::fire(const StateClass& elapsed, size_t t,
                                       StateClass& successor) const {
  std::vector<StateClass> successors = fire_all(elapsed, t);
  if (successors.empty()) {
    return false;
  }
  if (successors.size() != 1) {
    throw std::runtime_error(
        "firing has multiple admissible target active sets; use fire_all()");
  }
  successor = std::move(successors.front());
  return true;
}

}  // namespace state_class
