#ifndef ANALYSIS_REACHABILITY_SCHEDULING_FIFO_H
#define ANALYSIS_REACHABILITY_SCHEDULING_FIFO_H

#include <vector>

#include "analysis/reachability/scheduling/firing_rule.h"

namespace state_class {

// First-In First-Out scheduling under the release-clock state extension.
//
// Supported class (enforced by lowering and re-checked when the rule indexes a
// net): TDG2PN-lowered nets with `num_cpus >= 1`, `cores_per_cpu == 1`,
// statically partitioned tasks on cores `0 .. num_cpus-1`, every task chain
// transition on the task's own core, `task_place_capacity == 1`, no locks, no
// fork/join, no task-to-task dependency edges, at most one start token per
// task, and every task either periodic with a positive period or started once
// at time 0. Each task chain carries at most one in-flight job. Tasks never
// migrate between cores. FIFO is strictly non-preemptive: once a job leaves the
// entry it runs until its final execution segment places a token in the exit
// place.
//
// A job is pending while its token is at the task entry and running while its
// token is in any other non-exit chain place. At most one job runs per real
// core (the structural `coreN` place in the standard lowering, and the
// marking-derived running set here for nets without a CPU place):
//   - If a job is already running on a core, that core contributes only the
//     control transitions plus that job's enabled chain transitions. No pending
//     job on the same core may dispatch.
//   - Otherwise the core is free, and for each pending job on it whose start
//     is structurally enabled the rule emits one per-core choice containing
//     that candidate's chain transitions. The candidate is the earliest
//     arrival on its core:
//
//       r_candidate >= r_other  <=>  r_other - r_candidate <= 0
//                                      (BranchConstraint{candidate, other, 0})
//
//     where `r` is the per-task release clock (larger value = earlier arrival;
//     every in-flight job's release clock advances at rate 1). A choice whose
//     constraint set is inconsistent with the current zone is dropped. Ties
//     (equal release values) admit every tied candidate, so both dispatch
//     orders are enumerated rather than chosen by task index. A core with no
//     in-flight job contributes no choice. If a free core has pending jobs but
//     no feasible candidate, the rule raises an error.
//
// The per-core choices are Cartesian-producted with the global control
// transitions by combine_core_choices(), which includes each control transition
// once and concatenates the per-core constraint lists in ascending core order.
// Independent ties on different cores therefore produce the full product, and
// a busy core never blocks dispatch on another core. Release clocks of
// different cores are never compared.
//
// Release-clock lifecycle (shared with EDF):
//   - A release that fires while the task has no job creates a job whose
//     release clock starts at zero.
//   - A release merged into a job still waiting at the capacity-1 entry place
//     does not reset the pending job's release clock.
//   - A release that fires while the previous job is running (entry empty)
//     adds a second chain token. The supported class has one in-flight job per
//     task, so the resulting state is rejected with `std::invalid_argument`
//     instead of modeling a queue.
//   - A periodic release is deferred while a completed job's exit token is
//     pending; the zero-time consumer runs first.
//   - Completion drops the release clock.
//
// FIFO has no scalar priority; `effective_priority()` throws so no code path
// can silently fall back to static priorities. Preemption is disabled
// structurally: lowering uses the non-resume encoding (one `coreN` place per
// real core, capacity 1, and non-suspendable execution segments), and
// `preemption_mode()` is NON_PREEMPTIVE. The CPU places of different cores are
// independent; there is no migration.
class FifoFiringRule : public FiringRule {
 public:
  FifoFiringRule() = default;
  explicit FifoFiringRule(const petri::PTPN& net);

  int effective_priority(const PolicyContext& context, size_t transition) const override;

  bool admissible(const PolicyContext& /*context*/, size_t /*transition*/) const override {
    return true;
  }

  PreemptionMode preemption_mode() const override {
    return PreemptionMode::NON_PREEMPTIVE;
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

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_FIFO_H
