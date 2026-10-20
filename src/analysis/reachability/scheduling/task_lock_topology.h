#ifndef ANALYSIS_REACHABILITY_SCHEDULING_TASK_LOCK_TOPOLOGY_H
#define ANALYSIS_REACHABILITY_SCHEDULING_TASK_LOCK_TOPOLOGY_H

#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analysis/reachability/state_class.h"
#include "common/task_chain_layout.h"
#include "model/petri.h"

namespace state_class {

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
//   2. The chain layout comes from common/task_chain_layout.h (the header the
//      lowering builds the chains with): entry=0, get_core=1, ready=2, first
//      exec=3. Lock i is acquired by the transition at
//      `TaskChainLayout::lock_acquire_transition(i)` and released by the
//      transition at `lock_release_transition(chain.size(), i)` (the execution
//      segment that ends the critical section; its post arc returns the lock
//      token).
//   3. A task holds lock i when its token position `p` satisfies
//      `acquire_position + 1 <= p <= release_position - 1` and the shared lock
//      place is empty. The range covers the complete critical section.
//   4. A task requests lock i when `p == acquire_position - 1`. The request is
//      a *direct block* if the acquire transition is not structurally enabled
//      (the lock is held), and a *free request* otherwise (the lock is
//      available and the ceiling rule decides admission).
class TaskLockTopology {
 public:
  // From the shared layout header (ptpn::TaskChainLayout).
  static constexpr size_t kLockAcquireBase = ptpn::TaskChainLayout::kLockAcquireBase;
  static constexpr size_t kLockStride = ptpn::TaskChainLayout::kLockStride;
  static constexpr size_t kLockReleaseSuffix = ptpn::TaskChainLayout::kLockReleaseSuffix;
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

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_TASK_LOCK_TOPOLOGY_H
