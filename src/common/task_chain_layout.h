#ifndef COMMON_TASK_CHAIN_LAYOUT_H
#define COMMON_TASK_CHAIN_LAYOUT_H

#include <cstddef>

namespace ptpn {

// Layout of the task chain that TDG2PN lowering builds and that the analysis
// layer reads back (dynamic lock protocols, preemption). Both layers include
// this header instead of mirroring the numbers, so the layout cannot drift
// silently.
//
//   index 0: entry place
//   index 1: get_core transition
//   index 2: ready place
//   then, for execution segment s = 0, 1, ...: the execution transition, its
//   successor place, and — while locks remain — the lock-acquire transition
//   and its hold place.
//
// Lock i is acquired by the transition at lock_acquire_transition(i) and
// released by the execution transition at lock_release_transition(chain_size,
// i) (its post arc returns the lock token), so the critical section of lock i
// is the execution segment whose transition lies in (acquire, release].
struct TaskChainLayout {
  static constexpr std::size_t kEntry = 0;
  static constexpr std::size_t kGetCore = 1;
  static constexpr std::size_t kReady = 2;
  static constexpr std::size_t kFirstExec = 3;
  static constexpr std::size_t kFirstSegDone = 4;
  static constexpr std::size_t kMinLength = 5;
  // Acquire position of lock 0 and the stride between consecutive acquires.
  static constexpr std::size_t kLockAcquireBase = 5;
  static constexpr std::size_t kLockStride = 4;
  // Distance from the end of the chain to the release transition of lock 0
  // (plus two per additional lock: locks are released in reverse order).
  static constexpr std::size_t kLockReleaseSuffix = 4;

  static constexpr std::size_t lock_acquire_transition(std::size_t lock_index) {
    return kLockAcquireBase + kLockStride * lock_index;
  }

  static constexpr std::size_t lock_release_transition(std::size_t chain_length,
                                                       std::size_t lock_index) {
    return chain_length - kLockReleaseSuffix - 2 * lock_index;
  }
};

}  // namespace ptpn

#endif  // COMMON_TASK_CHAIN_LAYOUT_H
