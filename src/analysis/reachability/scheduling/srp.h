#ifndef ANALYSIS_REACHABILITY_SCHEDULING_SRP_H
#define ANALYSIS_REACHABILITY_SCHEDULING_SRP_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "analysis/reachability/scheduling/firing_rule.h"
#include "analysis/reachability/scheduling/task_lock_topology.h"

namespace state_class {

// Stack Resource Policy as a dynamic firing rule.
//
// This is a specified fixed-priority SRP variant for this model:
//
//   1. Preemption level = base priority = input `TaskInfo::priority`. The same
//      value is the scheduling priority. SRP does **not** auto-combine with the
//      RM/DM priority computation in this phase; a user who wants
//      deadline-monotonic SRP must supply deadline-ordered priorities. The
//      classical SRP requirement that preemption levels be deadline-consistent
//      is the caller's responsibility.
//   2. ceiling(L) = maximum base priority over every task whose
//      `TaskInfo::locks` contains L (detail::TaskLockTopology::lock_ceiling).
//      A lock no task uses has no ceiling.
//   3. The system ceiling of a marking is the maximum ceiling over all locks
//      currently held by any task (unique_holder + lock_ceiling). It is global
//      across the net, not per core. If no lock is held it is empty; there is
//      no 0 sentinel.
//   4. Start gate: a task's `get_core` transition (chain position 1) is
//      admissible only while the task's single job token is still at the chain
//      entry (position 0) and either no lock is held or
//      `base_priority(task) > system_ceiling` (strict). If the gate rejects,
//      the task stays at entry. Every other transition is admissible.
//   5. A task whose token is past position 0 has started; SRP never blocks it
//      again, even if the system ceiling rises later (subject to the ordinary
//      structural lock gating). There is no PCP-style admission at lock
//      acquire. Nested locks are therefore permitted for a running task.
//   6. No priority inheritance: `effective_priority` is the transition's
//      static priority. A lock holder never inherits a waiter's priority; the
//      ceiling start gate is the only dynamic restriction.
//
// Preemption uses the resume encoding (RESUME).
//
// Supported class: TDG2PN-lowered SRP nets with the standard chain layout,
// `task_place_capacity == 1`, at most one start token per task, and exactly one
// job token per task chain in every analyzed state. Lowering rejects larger
// capacities / start counts. Unsupported or ambiguous markings (zero or
// multiple tokens in a chain, inconsistent lock ownership, wrong marking size)
// skip the admission check and are not certified as SRP-exact: this is a
// defensive fallback, not a proven bound. Hand-written `.ptpn` nets default to
// FIXED, so SRP is only reachable through lowering.
//
// Multi-core scope: the system ceiling is global across the net, not per core.
// Classical SRP is a uniprocessor protocol; results for multi-core models are
// for this specified global-ceiling variant.
class SrpFiringRule : public FiringRule {
 public:
  SrpFiringRule() = default;
  explicit SrpFiringRule(const petri::PTPN& net);

  int effective_priority(const PolicyContext& context, size_t transition) const override;
  bool admissible(const PolicyContext& context, size_t transition) const override;

  PreemptionMode preemption_mode() const override {
    return PreemptionMode::RESUME;
  }

  bool supports_completion_observation() const override {
    return true;
  }

  // Maximum base priority among `TaskInfo::locks` users of `lock`; std::nullopt
  // when no task uses it. Exposed for diagnostics/tests.
  [[nodiscard]] std::optional<int> lock_ceiling(const std::string& lock) const;

  // Chain positions mirrored from converter::detail::TaskChainLayout.
  static constexpr size_t kLockAcquireBase = detail::TaskLockTopology::kLockAcquireBase;
  static constexpr size_t kLockStride = detail::TaskLockTopology::kLockStride;
  static constexpr size_t kLockReleaseSuffix = detail::TaskLockTopology::kLockReleaseSuffix;

 private:
  void ensure_index(const petri::PTPN& net) const;
  const detail::TaskLockTopology::MarkingState& dynamic_state(const petri::PTPN& net,
                                                              const petri::Marking& marking) const;

  mutable std::unique_ptr<detail::TaskLockTopology> index_;
  // One-entry memo of the marking analysis used by admissible(). The same
  // memo is shared with effective_priority() (which is static and therefore
  // does not need marking data, but must not build a second analysis).
  mutable detail::TaskLockTopology::MarkingState cached_layout_;
  mutable petri::Marking cached_marking_;
  mutable const petri::PTPN* cached_net_ = nullptr;
  mutable bool cache_valid_ = false;
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_SRP_H
