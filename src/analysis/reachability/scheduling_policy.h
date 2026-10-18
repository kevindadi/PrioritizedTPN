#ifndef ANALYSIS_REACHABILITY_SCHEDULING_POLICY_H
#define ANALYSIS_REACHABILITY_SCHEDULING_POLICY_H

#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "model/petri.h"

namespace state_class {

// `StateClass` is only referenced by pointer here (dynamic rules may read the
// marking/zone of the state being expanded). Keeping the forward declaration
// avoids an include cycle with state_class.h.
class StateClass;

// How a filtered-out transition is treated by the analysis engine.
enum class PreemptionMode {
  RESUME,         // freeze the execution clock; resume from the frozen value
  RESTART,        // structural restart encoding (see lowered preempt sub-net)
  NON_PREEMPTIVE  // no preemption; policy not otherwise implemented
};

// Inputs available to a firing rule when priorities/admissibility are computed.
struct PolicyContext {
  const petri::PTPN& net;
  // Marking is always set; the full clock layout/zone may be absent at the
  // point priority branches are computed. Static rules must not read it.
  const StateClass* state = nullptr;
};

// A scheduling policy expressed as a priority + admissibility function over
// the transitions of a PTPN. Higher `effective_priority` means higher
// scheduling priority. Implementations must be deterministic.
class FiringRule {
 public:
  virtual ~FiringRule() = default;

  virtual int effective_priority(const PolicyContext& context, size_t transition) const = 0;

  // Optional admissibility constraint (resource protocols, non-preemptive
  // running task). Default: every structurally enabled transition is
  // admissible.
  virtual bool admissible(const PolicyContext& context, size_t transition) const;

  virtual PreemptionMode preemption_mode() const = 0;
};

namespace detail {

// Shared task-chain and lock topology used by the dynamic lock protocols
// (PIP and PCP). The analysis layer deliberately does not include the lowering
// headers; the constants mirror converter::detail::TaskChainLayout.
//
// A task chain carries one job token. Ownership is derived from the chain
// position of that token, not from hold-place occupancy:
//
//   1. Count tokens over *every* place of the task chain, including the
//      lock-hold places. A supported state has exactly one token in the chain;
//      zero or multiple tokens make the task unsupported (defensive fallback:
//      it keeps its base priority and contributes no holder claim).
//   2. The chain layout mirrors `converter::detail::TaskChainLayout`: entry=0,
//      get_core=1, ready=2, first exec=3. Lock i is acquired by the transition
//      at position `5 + 4*i` and released by the transition at
//      `chain.size() - 4 - 2*i` (the execution segment that ends the critical
//      section; its post arc returns the lock token).
//   3. A task holds lock i when its token position `p` satisfies
//      `acquire_position + 1 <= p <= release_position - 1` and the shared lock
//      place is empty. The range covers the complete critical section.
//   4. A task requests lock i when `p == acquire_position - 1`. The request is
//      a *direct block* if the acquire transition is not structurally enabled
//      (the lock is held), and a *free request* otherwise (the lock is
//      available and the ceiling rule decides admission).
class TaskLockTopology {
 public:
  // Mirrored from converter::detail::TaskChainLayout.
  static constexpr size_t kLockAcquireBase = 5;
  static constexpr size_t kLockStride = 4;
  static constexpr size_t kLockReleaseSuffix = 4;
  // Sentinel for MarkingState::token_position when the task is unsupported.
  static constexpr size_t kNoChainPosition = std::numeric_limits<size_t>::max();

  struct LockBinding {
    std::string lock;
    size_t lock_place = 0;
    size_t acquire_position = 0;
    size_t acquire_transition = 0;
    size_t release_position = 0;
  };

  struct TaskRecord {
    std::string name;
    int base_priority = 0;
    // Full lowered chain and the place -> chain-position map used to locate
    // the single job token.
    std::vector<petri::NodeRef> chain;
    std::vector<size_t> chain_places;
    std::unordered_map<size_t, size_t> position_of_place;
    std::vector<size_t> chain_transitions;
    std::vector<LockBinding> locks;
  };

  // Layout facts derived from one marking.
  struct MarkingState {
    // Exactly one job token in the chain (the supported class).
    std::vector<bool> supported;
    // Chain position of that unique job token (entry = 0, get_core transition =
    // 1, ready = 2, ...); kNoChainPosition when the task is unsupported.
    std::vector<size_t> token_position;
    // Lock -> unique supported holder. A lock is present only when its shared
    // place is empty and exactly one supported task's token range covers it;
    // inconsistent markings (non-empty place, multiple claimants) are omitted.
    std::map<std::string, int> unique_holder;
    // Supported tasks parked on an acquire pre-place whose acquire transition
    // is not structurally enabled: (task, requested lock). Direct blocking.
    std::vector<std::pair<int, std::string>> blocked_requests;
    // Supported tasks parked on an acquire pre-place whose acquire transition
    // is structurally enabled: (task, requested free lock). The ceiling rule
    // decides whether the task is admissible.
    std::vector<std::pair<int, std::string>> free_requests;
  };

  void build(const petri::PTPN& net);

  [[nodiscard]] bool built_for(const petri::PTPN& net) const {
    return net_ == &net;
  }

  [[nodiscard]] MarkingState analyze_marking(const petri::Marking& marking) const;

  // Maximum base priority among tasks whose `TaskInfo::locks` contains `lock`.
  // std::nullopt when no task uses the lock (it then has no ceiling).
  [[nodiscard]] std::optional<int> lock_ceiling(const std::string& lock) const;

  // Maximum ceiling of locks currently held by tasks other than `task`.
  // std::nullopt means "no other task holds a lock": admission is always
  // allowed. Zero is a real ceiling and is never used as an empty sentinel.
  [[nodiscard]] std::optional<int> system_ceiling_excluding(const MarkingState& state,
                                                            int task) const;

  // Maximum ceiling over every lock currently held by any supported task.
  // std::nullopt means no lock is held (empty system ceiling); zero is a real
  // ceiling and is never used as an empty sentinel.
  [[nodiscard]] std::optional<int> system_ceiling(const MarkingState& state) const;

  [[nodiscard]] const std::vector<TaskRecord>& tasks() const {
    return tasks_;
  }

  // transition -> ordinal, or -1 for control/other transitions.
  [[nodiscard]] const std::vector<int>& task_of_transition() const {
    return task_of_transition_;
  }

  // transition -> (task ordinal, index into TaskRecord::locks) for the lock
  // acquire transitions only.
  [[nodiscard]] const std::unordered_map<size_t, std::pair<int, size_t>>& acquire_transitions()
      const {
    return acquire_transitions_;
  }

  [[nodiscard]] const petri::PTPN* net() const {
    return net_;
  }

 private:
  const petri::PTPN* net_ = nullptr;
  std::vector<TaskRecord> tasks_;
  std::vector<int> task_of_transition_;
  std::unordered_map<size_t, std::pair<int, size_t>> acquire_transitions_;
  std::map<std::string, int> lock_ceilings_;
};

}  // namespace detail

// Reads the priority statically encoded on each PTPN transition. This covers
// FIXED/FIXED_PRIOR_WITH_RESUME/FIXED_PRIOR_WITH_RESTART and RM/DM whose
// priorities are computed during lowering and stored on the transitions. The
// remaining policies (EDF/LLF/FIFO) keep their static priorities and are not
// otherwise analyzed yet.
class StaticPriorityRule : public FiringRule {
 public:
  explicit StaticPriorityRule(PreemptionMode mode) : mode_(mode) {}

  int effective_priority(const PolicyContext& context, size_t transition) const override;
  bool admissible(const PolicyContext& context, size_t transition) const override;
  PreemptionMode preemption_mode() const override {
    return mode_;
  }

 private:
  PreemptionMode mode_;
};

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

// Policy -> rule mapping:
//   FIXED, FIXED_PRIOR_WITH_RESUME, RM, DM -> static priorities, RESUME
//   PIP                                   -> PipFiringRule, RESUME
//   FIXED_PRIOR_WITH_RESTART              -> static priorities, RESTART
//   PCP, SRP, EDF, LLF, FIFO              -> static priorities, NON_PREEMPTIVE
//
// The policy-only overload cannot precompute the dynamic protocol's task-chain
// index; it returns a rule that lazily indexes `PolicyContext::net` on first
// use. Use the net-aware overload when the net is available.
std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy);
std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy, const petri::PTPN& net);

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_POLICY_H
