#ifndef ANALYSIS_REACHABILITY_SCHEDULING_LLF_H
#define ANALYSIS_REACHABILITY_SCHEDULING_LLF_H

#include <vector>

#include "analysis/reachability/scheduling/firing_rule.h"

namespace state_class {

// Event-driven Least Laxity First (LLF-ED) under the laxity-clock state
// extension.
//
// This is NOT classical continuous-time LLF. A scheduling decision is taken
// only at discrete state-class events (a release, a completion, or an execution
// segment boundary); running jobs are never preempted inside a
// `time_elapse` region merely because their laxities cross. Within such a
// region the selected active transition stays active and every other execution
// clock is frozen, so the decision is exactly the event-driven variant.
//
// Urgency clock. For a job j with release age A_j, remaining execution budget
// R_j = C_j - executed_j, and absolute deadline d_j:
//
//   chi_j = -laxity_j = A_j + R_j - d_j
//
// with C_j = TaskInfo::wcet and d_j = TaskInfo::deadline (the positive
// implicit period when no explicit deadline is given). Larger chi means more
// urgent. At release chi_j = C_j - d_j, which may be negative. The laxity clock
// is a general DBM variable, not pinned to zero.
//
// Rate law during time elapse:
//   - rate 0 while the job is actively executing a deterministic execution
//     segment (that execution transition is in `priority_enabled`);
//   - rate 1 otherwise: waiting at the entry place, released but filtered out
//     of the active set, suspended/preempted, between execution segments, or
//     while a zero-time control step is selected.
// Age and remaining budget move in opposite directions while executing, so chi
// is continuous across a deterministic segment firing; the clock is carried
// unchanged rather than reset. It is never capped.
//
// Dispatch and branch enumeration. Tasks are statically partitioned, so LLF-ED
// is applied independently on every real core. On each core the rule emits one
// per-core choice per feasible maximum-chi in-flight candidate i on that core.
// Each choice activates i's structurally enabled chain transitions, and every
// other in-flight candidate j *on the same core* contributes
//
//   chi_j - chi_i <= 0        (BranchConstraint{i, j, 0, ClockKind::Laxity})
//
// Choices inconsistent with the current zone are dropped. Equal chi admits
// every tied candidate (non-strict constraint), so ties are enumerated rather
// than broken by task index. Laxity clocks of different cores are never
// compared. A core with no in-flight job contributes no choice; if an
// in-flight core has no feasible candidate the rule raises an error.
// combine_core_choices() takes the Cartesian product of the per-core choices
// and includes the global control transitions once. A job that is not selected
// on its core is preempted (its execution clock freezes) and resumes later;
// `preemption_mode()` is RESUME. Tasks never migrate between cores.
//
// Supported class (enforced by lowering and re-checked when the rule indexes a
// net): TDG2PN-lowered LLF nets with `num_cpus >= 1`, `cores_per_cpu == 1`,
// statically partitioned tasks on cores `0 .. num_cpus-1`, `task_place_capacity
// == 1`, no locks, no fork/join, no task-to-task dependency edges, at most one
// start token per task, every task either periodic with a positive period or
// started once at time 0, a positive absolute deadline (or a positive period
// supplying the implicit deadline), at most one in-flight job per task, and
// deterministic execution segments (`earliest == latest`). Interval execution
// times are rejected because then the executed work would not be determined by
// elapsed time and chi could not be carried exactly across a segment firing.
// Overlapping releases are rejected as unsupported when the release fires while
// the task is mid-chain; a release saturating a still-pending entry token
// starts no new job, so the pending job keeps its original release age and
// laxity clock, matching the net's capacity-one merge semantics. Completed jobs
// drop their laxity clock.
//
// LLF-ED has no scalar priority; `effective_priority()` throws so the analysis
// can never silently fall back to static priorities.
class LlfFiringRule : public FiringRule {
 public:
  LlfFiringRule() = default;
  explicit LlfFiringRule(const petri::PTPN& net);

  int effective_priority(const PolicyContext& context, size_t transition) const override;

  bool admissible(const PolicyContext& /*context*/, size_t /*transition*/) const override {
    return true;
  }

  PreemptionMode preemption_mode() const override {
    return PreemptionMode::RESUME;
  }

  bool uses_laxity_clocks() const override {
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

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_LLF_H
