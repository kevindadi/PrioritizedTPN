#ifndef ANALYSIS_REACHABILITY_SCHEDULING_EDF_H
#define ANALYSIS_REACHABILITY_SCHEDULING_EDF_H

#include <vector>

#include "analysis/reachability/scheduling/firing_rule.h"

namespace state_class {

// Earliest Deadline First under the release-clock state extension.
//
// Supported class (enforced by lowering and re-checked when the rule indexes a
// net): TDG2PN-lowered nets with `num_cpus >= 1`, `cores_per_cpu == 1`,
// statically partitioned tasks on cores `0 .. num_cpus-1`, every task chain
// transition on the task's own core, `task_place_capacity == 1`, no locks, no
// fork/join, no task-to-task dependency edges, at most one start token per
// task, and every task either periodic with a positive period/deadline or
// started once at time 0 with a positive deadline. Each task chain carries at
// most one in-flight job. Tasks never migrate between cores.
//
// A task is in flight from the release that places a token in its entry place
// until its final execution segment places a token in its exit place. The
// graph maintains one release clock r_i per in-flight task (ClockKind::Release,
// reset to zero at release, rate 1 while the job is in flight, removed at
// completion). The remaining deadline of job i is D_i - r_i.
//
// `branches()` selects a deadline-earliest task independently on every real
// core: for each core it considers only the in-flight tasks statically bound to
// that core. For every task i that can be earliest on its core in some
// valuation of the current zone it emits one per-core choice containing i's
// structurally enabled chain transitions, constrained by
//
//   D_i - r_i <= D_j - r_j  for every other in-flight task j on the same core
//   <=>  r_j - r_i <= D_j - D_i      (BranchConstraint{i, j, D_j - D_i})
//
// Release clocks of different cores are never compared. The per-core choices
// are then Cartesian-producted with the global control transitions by
// combine_core_choices(): one branch per combination, with the control
// transitions included once. The constraints are part of the successor zone
// before hashing, so orderings are never fixed inside a shared DBM zone.
// Equal remaining deadlines make both i and j feasible on that core; both tie
// resolutions are emitted and independent ties on different cores produce the
// full Cartesian product. If an in-flight core has no feasible candidate the
// rule raises an error rather than emitting a partial schedule. At capacity one
// no additional cutoff subset exists, so these are exactly the EDF choices.
//
// Periodic release (`_fire`) handling in the supported single-job class:
//   - A release that fires while the task has no job starts a new job; its
//     release clock is created at zero.
//   - A release that fires while the job is still waiting at the entry is
//     merged by the saturating task places (`task_place_capacity == 1`). It
//     starts no second job and does not reset the release clock: the in-flight
//     job keeps its original release instant and absolute deadline. This is
//     the conservative reading of the lowered saturation semantics.
//   - A release is not scheduled while a completed job's exit token is still
//     pending. The completion consumer is zero-time and always enabled, so
//     letting it fire first preserves every timing behavior while preventing
//     an unconsumed completion token from merging with the next completion.
//   - A release that fires while the job has left the entry but has not
//     completed adds a second job token to the chain (the entry is empty, so
//     saturation does not clamp it). The supported class has one in-flight job
//     per task, so the resulting state is rejected with std::invalid_argument
//     instead of modeling queued/overlapping releases. This is what a response
//     time exceeding the period looks like on the lowered net.
//
// EDF has no scalar priority; `effective_priority()` throws so the analysis
// can never silently fall back to a static deadline priority. Preemption uses
// the resume encoding (RESUME): a preempted execution clock freezes while its
// release clock keeps running.
class EdfFiringRule : public FiringRule {
 public:
  EdfFiringRule() = default;
  explicit EdfFiringRule(const petri::PTPN& net);

  int effective_priority(const PolicyContext& context, size_t transition) const override;

  bool admissible(const PolicyContext& context, size_t transition) const override {
    return true;
  }

  PreemptionMode preemption_mode() const override {
    return PreemptionMode::RESUME;
  }

  bool uses_release_clocks() const override {
    return true;
  }

  std::vector<PolicyBranch> branches(const TransitionSet& struct_enabled, const petri::PTPN& net,
                                     const PolicyContext& context) const override;

  // Name-sorted task chains and transition -> task ordinal map. Exposed for
  // diagnostics and tests.
  [[nodiscard]] const std::vector<TaskChain>& tasks() const {
    return tasks_;
  }

  [[nodiscard]] const std::vector<int>& task_of_transition() const {
    return task_of_transition_;
  }

  // transition -> task ordinal for period-release (`_fire`) transitions, or -1.
  [[nodiscard]] const std::vector<int>& task_of_periodic_transition() const {
    return task_of_periodic_transition_;
  }

 private:
  // Validates the supported class and builds the chain index.
  void ensure_index(const petri::PTPN& net) const;

  mutable const petri::PTPN* net_ = nullptr;
  mutable std::vector<TaskChain> tasks_;
  mutable std::vector<int> task_of_transition_;
  mutable std::vector<int> task_of_periodic_transition_;
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_EDF_H
