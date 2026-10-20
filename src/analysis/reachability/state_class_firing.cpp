#include <algorithm>
#include <stdexcept>
#include <string>

#include "analysis/dbm/clock_state.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"

namespace state_class {

namespace {

// Intersects the job-clock scheduling constraints with the successor zone.
// Returns false when the branch is infeasible and must be dropped.
bool apply_branch_constraints(StateClass& state, const std::vector<BranchConstraint>& constraints) {
  for (const BranchConstraint& constraint : constraints) {
    if (!uses_job_clock_kind(constraint.domain)) {
      throw std::invalid_argument("branch constraint must use the release or laxity clock domain");
    }
    const int left = job_clock_index(state, constraint.domain, constraint.left_task);
    const int right = job_clock_index(state, constraint.domain, constraint.right_task);
    if (left <= 0 || right <= 0) {
      throw std::invalid_argument(std::string(clock_kind_label(constraint.domain)) +
                                  "-clock constraint references a task without an in-flight job");
    }
    // job_clock[right] - job_clock[left] <= bound.
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
  // V_run = { r_i : i in flight }
  //         U { chi_i : task i is not actively executing a segment }
  //         U { h_t : t in E_pri }
  //         U { w_t : t in suspended }.
  //
  // LLF rate law: chi = age + remaining_budget - deadline. While a job executes
  // a deterministic segment, age grows at rate 1 and remaining budget falls at
  // rate 1, so chi is frozen. Otherwise chi advances at rate 1. Because the
  // rate is zero for the whole time the task's execution transition is active,
  // chi is also continuous across deterministic segment boundaries.
  bool has_laxity = false;
  for (size_t i = 1; i < n && i < out.clock_vars.size(); ++i) {
    if (out.clock_vars[i].kind == ClockKind::Laxity) {
      has_laxity = true;
      break;
    }
  }
  std::vector<bool> executing_task;
  if (has_laxity) {
    const std::vector<int>& exec_task = execution_task_of_transition();
    executing_task.assign(task_chains().size(), false);
    for (size_t t : out.priority_enabled) {
      if (t < exec_task.size() && exec_task[t] >= 0) {
        executing_task[static_cast<size_t>(exec_task[t])] = true;
      }
    }
  }

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
    } else if (var.kind == ClockKind::Laxity) {
      // Rate 0 while this task is actively executing its next deterministic
      // segment; rate 1 while waiting, filtered out/preempted, suspended,
      // between segments, or during a zero-time control step.
      running[i] = var.task >= executing_task.size() || !executing_task[var.task];
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
  // still-in-flight release/laxity clock keeps its job value; a still-suspended
  // clock inherits its suspension clock). Everything else is freshly created:
  // transition clocks start at zero and a fresh laxity clock starts at the
  // exact release value chi = wcet - deadline.
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
    } else if (var.kind == ClockKind::Laxity) {
      // Laxity is carried continuously across every firing. A newly in-flight
      // job resets to chi = wcet - deadline below. A merged release creates no
      // new job, so the pending job's clock is carried unchanged. Completed
      // jobs have no successor laxity clock and their column is dropped.
      source_index[i] = source.laxity_index(var.task);
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

  // Transition clocks and fresh release clocks start at zero. A fresh laxity
  // clock is pinned exactly to chi = wcet - deadline with two difference
  // constraints; it is never pinned to x0 and is never capped.
  for (size_t i = 1; i < n; ++i) {
    if (source_index[i] < 0) {
      if (successor.clock_vars[i].kind == ClockKind::Laxity) {
        const int chi = laxity_reset_value(successor.clock_vars[i].task);
        zone.set_constraint(0, i, -chi);
        zone.set_constraint(i, 0, chi);
      } else {
        zone.set_constraint(0, i, 0);
        zone.set_constraint(i, 0, 0);
      }
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
  const petri::Marking marking = petri::PTPN::fire(elapsed.marking, net_, petri::TransitionId{t});
  const TransitionSet struct_enabled = Scheduling::structural_enabled(net_, marking);

  // Rules that read job clocks need a prepared successor state: the job-clock
  // prefix depends only on the successor marking, so a probe built with the
  // full structural set has the same clock indices as every final branch. Its
  // zone is the unsplit successor zone (surviving clocks carried over, fresh
  // clocks zero except fresh laxity clocks at chi = wcet - deadline);
  // EDF/FIFO/LLF test each candidate ordering against it and emit the selected
  // difference constraints.
  StateClass prepared;
  prepared.marking = marking;
  if (firing_rule_->uses_job_clocks()) {
    recompute_sets(prepared, struct_enabled);
    build_layout(prepared);
    build_successor_zone(prepared, fired, elapsed, t);
  }
  // `elapsed` is a copy of the source state, so its selected active set is the
  // scheduler's running set for the tie rule.
  const PolicyContext context{net_, &prepared, &elapsed.priority_enabled};
  const std::vector<PolicyBranch> branches = firing_rule_->branches(struct_enabled, net_, context);

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
    throw std::runtime_error("firing has multiple admissible target active sets; use fire_all()");
  }
  successor = std::move(successors.front());
  return true;
}

}  // namespace state_class
