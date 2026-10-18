#include <algorithm>
#include <stdexcept>

#include "analysis/dbm/clock_state.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"

namespace state_class {

namespace {

// Pins every clock to the reference variable x0 (value zero).
DBM all_clock_zeros(size_t clock_count) {
  DBM zone(clock_count);
  for (size_t i = 1; i < clock_count; ++i) {
    zone.set_constraint(0, i, 0);
    zone.set_constraint(i, 0, 0);
  }
  zone.minimize();
  return zone;
}

// Intersects the release-clock scheduling constraints with the state
// zone. Returns
// false when the branch is infeasible and must be dropped.
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

int StateClassReachabilityGraph::effective_earliest(size_t transition) const {
  return net_.get_transition(petri::TransitionId{transition}).time_interval.effective_earliest();
}

int StateClassReachabilityGraph::effective_latest(size_t transition) const {
  const int latest =
      net_.get_transition(petri::TransitionId{transition}).time_interval.effective_latest();
  return latest == petri::INF ? INF_TIME : latest;
}

void StateClassReachabilityGraph::recompute_sets(StateClass& state,
                                                 const TransitionSet& active) const {
  state.struct_enabled = Scheduling::structural_enabled(net_, state.marking);
  state.priority_enabled = active;
  std::sort(state.priority_enabled.begin(), state.priority_enabled.end());

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

void build_clock_layout(StateClass& state, const petri::PTPN& net,
                        bool keep_suspension_clocks, bool include_release_clocks) {
  state.clock_vars.clear();
  state.clock_vars.push_back({ClockKind::Zero, NO_TRANSITION, NO_TASK});
  state.exec_clock_of_transition.assign(net.num_transitions(), -1);
  state.susp_clock_of_transition.assign(net.num_transitions(), -1);
  state.release_clock_of_task.clear();

  if (include_release_clocks) {
    // Release clocks are placed immediately after x0, ordered by task ordinal.
    // Their indices therefore do not depend on the active set, which keeps the
    // release-clock branch constraints stable across branch layouts.
    const std::vector<TaskChain> tasks = build_task_chains(net);
    state.release_clock_of_task.assign(tasks.size(), -1);
    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      if (!task_in_flight(tasks[ordinal], state.marking)) {
        continue;
      }
      const int release_idx = static_cast<int>(state.clock_vars.size());
      state.clock_vars.push_back(
          {ClockKind::Release, NO_TRANSITION, ordinal});
      state.release_clock_of_task[ordinal] = release_idx;
    }
  }

  for (size_t t : state.struct_enabled) {
    const int exec_idx = static_cast<int>(state.clock_vars.size());
    state.clock_vars.push_back({ClockKind::Execution, t});
    state.exec_clock_of_transition[t] = exec_idx;

    // Suspension clocks are projected out by default: w_t appears in no firing
    // guard, deadline cap or metric, so the `suspended` set alone carries all
    // scheduler information the analysis needs.
    if (keep_suspension_clocks && contains(state.suspended, t)) {
      const int susp_idx = static_cast<int>(state.clock_vars.size());
      state.clock_vars.push_back({ClockKind::Suspension, t});
      state.susp_clock_of_transition[t] = susp_idx;
    }
  }
}

void StateClassReachabilityGraph::build_layout(StateClass& state) const {
  build_clock_layout(state, net_, !suspension_clock_projection_,
                     firing_rule_->uses_release_clocks());
}

StateClass StateClassReachabilityGraph::compute_initial_class() {
  std::vector<StateClass> initials = compute_initial_classes();
  if (initials.size() != 1) {
    throw std::runtime_error(
        "initial marking has multiple admissible active sets; use "
        "compute_initial_classes()");
  }
  return std::move(initials.front());
}

std::vector<StateClass> StateClassReachabilityGraph::compute_initial_classes() {
  StateClass base;
  base.marking = net_.get_marking();
  base.elapsed_time = 0.0;

  const TransitionSet struct_enabled = Scheduling::structural_enabled(net_, base.marking);

  // Rules that use release clocks need a prepared layout/zone to derive the
  // deadline ordering. The release-clock prefix depends only on the marking,
  // so a probe built with the full structural set has the same release indices
  // as every final branch.
  StateClass prepared = base;
  prepared.struct_enabled = struct_enabled;
  prepared.priority_enabled = struct_enabled;
  if (firing_rule_->uses_release_clocks()) {
    build_layout(prepared);
    prepared.zone = all_clock_zeros(prepared.clock_vars.size());
  }
  const PolicyContext context{net_, firing_rule_->uses_release_clocks() ? &prepared : &base};
  const std::vector<PolicyBranch> branches = firing_rule_->branches(struct_enabled, net_, context);

  std::vector<StateClass> out;
  out.reserve(branches.size());
  for (const PolicyBranch& branch : branches) {
    StateClass state = base;
    recompute_sets(state, branch.active);
    build_layout(state);

    // Every clock starts at zero, i.e. pinned to the reference variable x0.
    state.zone = all_clock_zeros(state.clock_vars.size());
    if (!apply_branch_constraints(state, branch.constraints)) {
      continue;  // ordering impossible in the all-zero initial zone
    }

    out.push_back(std::move(state));
  }

  return out;
}

}  // namespace state_class
