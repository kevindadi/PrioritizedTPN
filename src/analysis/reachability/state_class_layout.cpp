#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

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

// Initial DBM for a fresh state class. Execution, suspension, and release
// clocks start pinned to x0 (= 0). A laxity clock starts at
// chi = wcet - deadline, which may be negative; it is therefore pinned with two
// difference constraints rather than to x0.
DBM initial_zone(const StateClass& state, const petri::PTPN& net) {
  bool has_laxity = false;
  for (const ClockVar& var : state.clock_vars) {
    if (var.kind == ClockKind::Laxity) {
      has_laxity = true;
      break;
    }
  }
  if (!has_laxity) {
    return all_clock_zeros(state.clock_vars.size());
  }

  // Build from a raw DBM instead of overwriting entries of an
  // all-clock-zeros zone: the latter is already closed, so changing a pin with
  // set_constraint alone leaves the cross-clock entries stale (they can no
  // longer be relaxed upward by minimize) and can manufacture a spurious
  // negative cycle.
  DBM zone(state.clock_vars.size());
  const std::vector<TaskChain> tasks = build_task_chains(net);
  for (size_t index = 1; index < state.clock_vars.size(); ++index) {
    const ClockVar& var = state.clock_vars[index];
    int chi = 0;
    if (var.kind == ClockKind::Laxity) {
      if (var.task >= tasks.size()) {
        throw std::invalid_argument("laxity clock references an unknown task");
      }
      const long long value =
          static_cast<long long>(tasks[var.task].wcet) - tasks[var.task].deadline;
      if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("laxity initial value does not fit in int");
      }
      chi = static_cast<int>(value);
    }
    // clock - x0 <= chi and x0 - clock <= -chi. Non-laxity clocks are pinned
    // to zero, which is the same pair of constraints with chi == 0.
    zone.set_constraint(0, index, -chi);
    zone.set_constraint(index, 0, chi);
  }
  zone.minimize();
  return zone;
}

// Intersects the job-clock scheduling constraints with the state zone.
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

int StateClassReachabilityGraph::effective_earliest(size_t transition) const {
  return net_.get_transition(petri::TransitionId{transition}).time_interval.effective_earliest();
}

int StateClassReachabilityGraph::effective_latest(size_t transition) const {
  const int latest =
      net_.get_transition(petri::TransitionId{transition}).time_interval.effective_latest();
  return latest == petri::INF ? INF_TIME : latest;
}

const std::vector<TaskChain>& StateClassReachabilityGraph::task_chains() const {
  if (!task_chains_valid_) {
    task_chains_cache_ = build_task_chains(net_);
    task_chains_valid_ = true;
  }
  return task_chains_cache_;
}

const std::vector<int>& StateClassReachabilityGraph::execution_task_of_transition() const {
  if (!execution_task_valid_) {
    execution_task_of_transition_.assign(net_.num_transitions(), -1);
    const std::vector<TaskChain>& tasks = task_chains();
    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      const TaskChain& task = tasks[ordinal];
      // The standard no-lock chain has get_core at transition position 0 and
      // execution segments after it. Locks are outside the LLF supported class,
      // so positions 1.. are exactly the execution segments there.
      for (size_t position = 1; position < task.transitions.size(); ++position) {
        const size_t transition = task.transitions[position];
        if (transition >= net_.num_transitions()) {
          throw std::invalid_argument("task '" + task.name +
                                      "' references an out-of-range transition");
        }
        if (execution_task_of_transition_[transition] >= 0) {
          throw std::invalid_argument("task chains share transition T" +
                                      std::to_string(transition) +
                                      "; the laxity rate law requires disjoint execution segments");
        }
        execution_task_of_transition_[transition] = static_cast<int>(ordinal);
      }
    }
    execution_task_valid_ = true;
  }
  return execution_task_of_transition_;
}

int StateClassReachabilityGraph::laxity_reset_value(size_t task) const {
  const std::vector<TaskChain>& tasks = task_chains();
  if (task >= tasks.size()) {
    throw std::invalid_argument("laxity clock references an unknown task ordinal");
  }
  const long long value = static_cast<long long>(tasks[task].wcet) - tasks[task].deadline;
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
    throw std::invalid_argument("laxity initial value does not fit in int for task '" +
                                tasks[task].name + "'");
  }
  return static_cast<int>(value);
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

void build_clock_layout(StateClass& state, const petri::PTPN& net, bool keep_suspension_clocks,
                        bool include_release_clocks, bool include_laxity_clocks) {
  if (include_release_clocks && include_laxity_clocks) {
    throw std::invalid_argument(
        "a state class cannot track release and laxity clocks simultaneously");
  }
  state.clock_vars.clear();
  state.clock_vars.push_back({ClockKind::Zero, NO_TRANSITION, NO_TASK});
  state.exec_clock_of_transition.assign(net.num_transitions(), -1);
  state.susp_clock_of_transition.assign(net.num_transitions(), -1);
  state.release_clock_of_task.clear();
  state.laxity_clock_of_task.clear();

  if (include_release_clocks || include_laxity_clocks) {
    // Job clocks are placed immediately after x0, ordered by task ordinal.
    // Their indices therefore do not depend on the active set, which keeps the
    // job-clock branch constraints stable across branch layouts.
    const ClockKind kind = include_laxity_clocks ? ClockKind::Laxity : ClockKind::Release;
    const std::vector<TaskChain> tasks = build_task_chains(net);
    std::vector<int>& clock_of_task =
        include_laxity_clocks ? state.laxity_clock_of_task : state.release_clock_of_task;
    clock_of_task.assign(tasks.size(), -1);
    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      if (!task_in_flight(tasks[ordinal], state.marking)) {
        continue;
      }
      const int clock_idx = static_cast<int>(state.clock_vars.size());
      state.clock_vars.push_back({kind, NO_TRANSITION, ordinal});
      clock_of_task[ordinal] = clock_idx;
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
                     firing_rule_->uses_release_clocks(), firing_rule_->uses_laxity_clocks());
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

  // Rules that use job clocks need a prepared layout/zone to derive the
  // job ordering. The job-clock prefix depends only on the marking, so a probe
  // built with the full structural set has the same clock indices as every
  // final branch.
  StateClass prepared = base;
  prepared.struct_enabled = struct_enabled;
  prepared.priority_enabled = struct_enabled;
  if (firing_rule_->uses_job_clocks()) {
    build_layout(prepared);
    prepared.zone = initial_zone(prepared, net_);
  }
  const PolicyContext context{net_, firing_rule_->uses_job_clocks() ? &prepared : &base};
  const std::vector<PolicyBranch> branches = firing_rule_->branches(struct_enabled, net_, context);

  std::vector<StateClass> out;
  out.reserve(branches.size());
  for (const PolicyBranch& branch : branches) {
    StateClass state = base;
    recompute_sets(state, branch.active);
    build_layout(state);

    // Transition clocks start at zero. Release clocks start at zero too;
    // laxity clocks start at chi = wcet - deadline.
    state.zone = initial_zone(state, net_);
    if (!apply_branch_constraints(state, branch.constraints)) {
      continue;  // ordering impossible in the initial zone
    }

    out.push_back(std::move(state));
  }

  return out;
}

}  // namespace state_class
