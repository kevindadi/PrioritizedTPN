#ifndef LOWER_TDG2PN_TDG2PN_COMMON_H
#define LOWER_TDG2PN_TDG2PN_COMMON_H

#include <cstddef>
#include <string>

#include "model/petri.h"
#include "model/types.h"

namespace converter::detail {

constexpr int kControlTransitionPriority = 0;

// Indices into the per-task place/transition chain stored in node_pn_map.
struct TaskChainLayout {
  static constexpr size_t kEntry = 0;
  static constexpr size_t kGetCore = 1;
  static constexpr size_t kReady = 2;
  static constexpr size_t kFirstExec = 3;
  static constexpr size_t kFirstSegDone = 4;
  static constexpr size_t kMinLength = 5;

  static size_t lock_acquire_transition(size_t lock_index) {
    return 5 + 4 * lock_index;
  }

  static size_t lock_release_transition(size_t chain_length, size_t lock_index) {
    return chain_length - 4 - 2 * lock_index;
  }
};

inline petri::TimeInterval immediate_interval() {
  return petri::TimeInterval(0, 0);
}

// The resume policy (and the legacy "fixed" alias) no longer relies on the
// structural CPU-resource place or the preempt/suspended/resume sub-net.
// Preemption is expressed natively by the analysis engine (per-core priority
// filtering + execution-clock freeze), so its execution segments are marked
// suspendable. The restart policy keeps the structural encoding untouched.
inline bool is_resume_policy(SchedulePolicy policy) {
  return policy == SchedulePolicy::FIXED || policy == SchedulePolicy::FIXED_PRIOR_WITH_RESUME;
}

inline size_t add_control_transition(petri::PTPN& ptpn, const std::string& name,
                                     const petri::TimeInterval& interval = petri::TimeInterval(0,
                                                                                               0)) {
  return ptpn.add_transition(name, interval, kControlTransitionPriority,
                             petri::kControlTransitionCore, /*suspendable=*/false);
}

}  // namespace converter::detail

#endif  // LOWER_TDG2PN_TDG2PN_COMMON_H
