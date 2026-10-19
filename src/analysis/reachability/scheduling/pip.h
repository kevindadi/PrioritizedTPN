#ifndef ANALYSIS_REACHABILITY_SCHEDULING_PIP_H
#define ANALYSIS_REACHABILITY_SCHEDULING_PIP_H

#include <memory>
#include <vector>

#include "analysis/reachability/scheduling/firing_rule.h"
#include "analysis/reachability/scheduling/task_lock_topology.h"

namespace state_class {

// Priority Inheritance Protocol as a dynamic firing rule.
//
// Base priorities come from `net.task_info()`; the effective priority of a
// task is recomputed from the current marking.
//
// Ownership and waiting are derived by `detail::TaskLockTopology` from the
// chain position of each task's single job token:
//
//   1. A supported state has exactly one token in every task chain. Zero or
//      multiple tokens make the task unsupported (defensive fallback: it keeps
//      its base priority and contributes no inheritance).
//   2. A task holds lock i when its token lies in
//      `[acquire_position + 1, release_position - 1]` and the shared lock
//      place is empty.
//   3. A task waits for lock i when its token sits on the acquire transition's
//      pre-place and that transition is not structurally enabled. This is
//      checked against the marking (a lock-blocked acquire is not structurally
//      enabled).
//   4. Inheritance is the least fixpoint of
//      `eff[holder] = max(eff[holder], eff[waiter])` over all wait edges.
//
// PIP only changes priorities; every structurally enabled transition remains
// admissible, and preemption uses the resume encoding (the analysis engine
// freezes suspended execution clocks).
//
// Supported class: TDG2PN-lowered PIP nets. Lowering rejects
// `task_place_capacity != 1`, and the position-range rule is exact only when
// every task chain contains exactly one job token. Unsupported or inconsistent
// markings (zero/multiple tokens, multiple claimants for one lock, a non-empty
// lock place) are handled defensively by omitting inheritance; no WCRT
// guarantee is claimed for those states. Hand-written `.ptpn` nets default to
// FIXED, so PIP is only reachable through lowering.
class PipFiringRule : public FiringRule {
 public:
  PipFiringRule() = default;
  explicit PipFiringRule(const petri::PTPN& net);

  int effective_priority(const PolicyContext& context, size_t transition) const override;
  bool admissible(const PolicyContext& context, size_t transition) const override;

  PreemptionMode preemption_mode() const override {
    return PreemptionMode::RESUME;
  }

  // Chain positions mirrored from converter::detail::TaskChainLayout (the
  // analysis layer must not depend on the lowering headers).
  static constexpr size_t kLockAcquireBase = detail::TaskLockTopology::kLockAcquireBase;
  static constexpr size_t kLockStride = detail::TaskLockTopology::kLockStride;
  static constexpr size_t kLockReleaseSuffix = detail::TaskLockTopology::kLockReleaseSuffix;

 private:
  void ensure_index(const petri::PTPN& net) const;
  const std::vector<int>& effective_priorities(const petri::PTPN& net,
                                               const petri::Marking& marking) const;

  mutable std::unique_ptr<detail::TaskLockTopology> index_;
  // One-entry memo: priority_branches asks for the same state many times
  // (sorting, cutoff selection, the control-core scan).
  mutable petri::Marking cached_marking_;
  mutable const petri::PTPN* cached_net_ = nullptr;
  mutable std::vector<int> cached_effective_;
  mutable bool cache_valid_ = false;
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_PIP_H
