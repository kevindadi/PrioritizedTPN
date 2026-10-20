#ifndef ANALYSIS_REACHABILITY_SCHEDULING_PCP_H
#define ANALYSIS_REACHABILITY_SCHEDULING_PCP_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "analysis/reachability/scheduling/firing_rule.h"
#include "analysis/reachability/scheduling/task_lock_topology.h"

namespace state_class {

// Priority Ceiling Protocol as a dynamic firing rule.
//
// Base priorities are `TaskInfo::priority` from the input. They define lock
// ceilings and admission; effective (inherited) priorities drive scheduling.
//
//   1. ceiling(L) = max base priority of every task whose `TaskInfo::locks`
//      contains L. A lock no task uses has no ceiling.
//   2. Held locks use the same chain-position range ownership as PIP (single
//      token, lock place empty).
//   3. The system ceiling of task J is the maximum ceiling of locks currently
//      held by tasks other than J. If no other task holds a lock, admission is
//      always allowed (there is no 0 sentinel).
//   4. Admission (ceiling rule): J may fire the lock-acquire transition for L
//      only when L is free *and* `base_priority(J) > system_ceiling(J)`.
//      A lock held by another task is already structurally disabled; it is a
//      direct block, not an admission decision.
//   5. Direct blocking: J requesting a lock held by H adds the wait edge J->H.
//      Ceiling blocking: J requesting a free lock with
//      `base_priority(J) <= system_ceiling(J)` is blocked by the holder of a
//      held lock attaining the system ceiling. When several held locks share
//      the maximum ceiling, the lexicographically smallest lock name is chosen
//      deterministically; this defines the analyzed PCP instance. It adds the
//      wait edge J->that holder.
//   6. Inheritance is the least fixpoint of
//      `eff[holder] = max(eff[holder], eff[waiter])` over direct and ceiling
//      wait edges (at most task_count rounds; a cycle saturates at the
//      maximum). A task's effective priority applies to every transition in
//      its chain; control/non-task transitions keep their static priority.
//
// Nested locks are permitted: the system ceiling excludes J's own held locks,
// so J may acquire another free lock exactly when its base priority exceeds
// the ceiling of locks held by other tasks.
//
// Preemption uses the resume encoding (RESUME), like fixed/PIP. Supported
// class: TDG2PN-lowered PCP nets with the standard chain layout,
// `task_place_capacity == 1`, one start token per binding, and exactly one job
// token per task chain in every analyzed state. Lowering rejects larger
// capacities / start counts. Unsupported or inconsistent markings fall back
// defensively (no holder claim / inheritance for the affected task or lock);
// no WCRT guarantee is claimed for those states. Hand-written `.ptpn` nets
// default to FIXED, so PCP is only reachable through lowering.
class PcpFiringRule : public FiringRule {
 public:
  PcpFiringRule() = default;
  explicit PcpFiringRule(const petri::PTPN& net);

  int effective_priority(const PolicyContext& context, size_t transition) const override;
  bool admissible(const PolicyContext& context, size_t transition) const override;

  PreemptionMode preemption_mode() const override {
    return PreemptionMode::RESUME;
  }

  bool supports_completion_observation() const override {
    return true;
  }

  // Maximum base priority among `TaskInfo::locks` users of `lock`; std::nullopt
  // when no task uses it.
  [[nodiscard]] std::optional<int> lock_ceiling(const std::string& lock) const;

 private:
  struct DynamicState {
    std::vector<int> effective;
    detail::TaskLockTopology::MarkingState layout;
  };

  void ensure_index(const petri::PTPN& net) const;
  const DynamicState& dynamic_state(const petri::PTPN& net, const petri::Marking& marking) const;

  mutable std::unique_ptr<detail::TaskLockTopology> index_;
  // One-entry memo shared by effective_priority() and admissible(), which are
  // both queried repeatedly for the same marking by priority_branches().
  mutable DynamicState cached_dynamic_;
  mutable petri::Marking cached_marking_;
  mutable const petri::PTPN* cached_net_ = nullptr;
  mutable bool cache_valid_ = false;
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_PCP_H
