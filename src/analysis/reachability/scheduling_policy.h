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

#include "analysis/reachability/state_class.h"
#include "model/petri.h"

namespace state_class {

// Policy decisions are expressed over the state-class clock layout: EDF and
// FIFO read the per-task release clocks, and TransitionSet/TaskChain come from
// state_class.h.

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

// One difference constraint over per-job clocks attached to a scheduling
// branch. `domain` selects the clock kind; the default (Release) preserves the
// EDF/FIFO encoding `release[right] - release[left] <= bound`. Event-driven LLF
// uses `Laxity` and the bound 0: `chi[right] - chi[left] <= 0`. Task ids are
// ordinals into build_task_chains(net) (sorted by task name).
struct BranchConstraint {
  size_t left_task = 0;
  size_t right_task = 0;
  int bound = 0;
  ClockKind domain = ClockKind::Release;

  bool operator==(const BranchConstraint& other) const {
    return left_task == other.left_task && right_task == other.right_task &&
           bound == other.bound && domain == other.domain;
  }
};

// One admissible scheduling decision: the selected active transitions plus
// the zone constraints that select the deadline ordering this decision
// represents. Constraints are intersected with the successor zone before it is
// hashed or deduplicated.
struct PolicyBranch {
  TransitionSet active;
  std::vector<BranchConstraint> constraints;

  bool operator==(const PolicyBranch& other) const {
    return active == other.active && constraints == other.constraints;
  }
};

// One scheduling decision on a single real core, used by
// combine_core_choices(): the chain transitions selected on that core plus the
// job-clock constraints that select this candidate. The helper does not
// interpret the core id; callers group choices by TaskChain::core.
struct CoreChoice {
  TransitionSet active;
  std::vector<BranchConstraint> constraints;
};

// Cartesian product of the global control transitions and the per-core
// choices. Every resulting branch contains the control transitions once, the
// sorted union of the selected per-core active sets, and the concatenation of
// the per-core constraint lists in ascending core order.
//
// A core absent from `by_core` is idle and contributes no choice. A core
// present with an empty choice list is a caller error (the caller must have
// already established that at least one candidate on that core is feasible);
// this helper throws std::invalid_argument. Duplicate branches (same active set
// and same constraint list) are removed while preserving enumeration order.
std::vector<PolicyBranch> combine_core_choices(
    const TransitionSet& control,
    const std::map<int, std::vector<CoreChoice>>& by_core);

// A scheduling policy expressed as a priority + admissibility function over
// the transitions of a PTPN. Higher `effective_priority` means higher
// scheduling priority. Implementations must be deterministic.
//
// Policies whose decisions depend on clock values that must stay explicit in
// the state override `branches()` and one of the job-clock predicates instead
// of encoding a fake scalar priority: the analysis graph then creates a
// per-job clock (release for EDF/FIFO, laxity for Event-driven LLF) and splits
// each zone by the policy's ordering.
class FiringRule {
 public:
  virtual ~FiringRule() = default;

  virtual int effective_priority(const PolicyContext& context, size_t transition) const = 0;

  // Optional admissibility constraint (resource protocols, non-preemptive
  // running task). Default: every structurally enabled transition is
  // admissible.
  virtual bool admissible(const PolicyContext& context, size_t transition) const;

  virtual PreemptionMode preemption_mode() const = 0;

  // True when the rule depends on per-task release clocks and therefore the
  // graph must include them in the clock layout and state identity.
  virtual bool uses_release_clocks() const {
    return false;
  }

  // True when the rule depends on per-task laxity clocks (chi = -laxity).
  virtual bool uses_laxity_clocks() const {
    return false;
  }

  // True iff either per-job clock kind is tracked. Such states reject
  // extrapolation and symmetry reduction.
  bool uses_job_clocks() const {
    return uses_release_clocks() || uses_laxity_clocks();
  }

  // Every admissible branch at a marking. The default implementation is the
  // existing scalar-priority branch enumeration with no extra constraints.
  // Rules that use job clocks override this; `context.state` then carries the
  // prepared successor/source layout and zone.
  virtual std::vector<PolicyBranch> branches(const TransitionSet& struct_enabled,
                                             const petri::PTPN& net,
                                             const PolicyContext& context) const;
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
// priorities are computed during lowering and stored on the transitions. LLF
// and UNKNOWN keep their static priorities and are not otherwise analyzed yet.
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

  std::vector<PolicyBranch> branches(const TransitionSet& struct_enabled,
                                     const petri::PTPN& net,
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

// First-In First-Out scheduling under the release-clock state extension.
//
// Supported class (enforced by lowering and re-checked when the rule indexes a
// net): TDG2PN-lowered nets with `num_cpus == 1`, `cores_per_cpu == 1`, all
// tasks on core 0, `task_place_capacity == 1`, no locks, no fork/join, no
// task-to-task dependency edges, at most one start token per task, and every
// task either periodic with a positive period or started once at time 0. Each
// task chain carries at most one in-flight job. FIFO is strictly
// non-preemptive: once a job leaves the entry it runs until its final
// execution segment places a token in the exit place.
//
// A job is pending while its token is at the task entry and running while its
// token is in any other non-exit chain place. At most one job runs on the
// single real core (the structural `core0` place in the standard lowering, and
// the marking-derived running set here for nets without a CPU place):
//   - If a job is already running, every branch contains only the control
//     transitions plus that job's enabled chain transitions.
//   - Otherwise, for each pending job whose start is structurally enabled the
//     rule emits one branch containing the control transitions plus that
//     candidate's chain transitions. The candidate is the earliest arrival:
//
//       r_candidate >= r_other  <=>  r_other - r_candidate <= 0
//                                      (BranchConstraint{candidate, other, 0})
//
//     where `r` is the per-task release clock (larger value = earlier arrival;
//     every in-flight job's release clock advances at rate 1). A branch whose
//     constraint set is inconsistent with the current zone is dropped. Ties
//     (equal release values) admit every tied candidate, so both dispatch
//     orders are enumerated rather than chosen by task index.
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
// structurally: lowering uses the non-resume encoding (a `core0` place and
// non-suspendable execution segments), and `preemption_mode()` is
// NON_PREEMPTIVE.
//
// Multi-core scope: not supported. The supported class has exactly one real
// core, so there is no cross-core ordering to model.
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

  std::vector<PolicyBranch> branches(const TransitionSet& struct_enabled,
                                     const petri::PTPN& net,
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
// Dispatch and branch enumeration. At a dispatch event the rule emits one
// branch per feasible maximum-chi in-flight candidate i. Each branch activates
// the control transitions plus i's structurally enabled chain transitions, and
// every other in-flight candidate j contributes
//
//   chi_j - chi_i <= 0        (BranchConstraint{i, j, 0, ClockKind::Laxity})
//
// Branches inconsistent with the current zone are dropped. Equal chi admits
// every tied candidate (non-strict constraint), so ties are enumerated rather
// than broken by task index. A job that is not selected is preempted (its
// execution clock freezes) and resumes later; `preemption_mode()` is RESUME.
//
// Supported class (enforced by lowering and re-checked when the rule indexes a
// net): TDG2PN-lowered LLF nets with `num_cpus == 1`, `cores_per_cpu == 1`,
// every task on core 0, `task_place_capacity == 1`, no locks, no fork/join, no
// task-to-task dependency edges, at most one start token per task, every task
// either periodic with a positive period or started once at time 0, a positive
// absolute deadline (or a positive period supplying the implicit deadline),
// at most one in-flight job per task, and deterministic execution segments
// (`earliest == latest`). Interval execution times are rejected because then
// the executed work would not be determined by elapsed time and chi could not
// be carried exactly across a segment firing. Overlapping releases are
// rejected as unsupported when the release fires while the task is mid-chain;
// a release saturating a still-pending entry token starts no new job, so the
// pending job keeps its original release age and laxity clock, matching the
// net's capacity-one merge semantics. Completed jobs drop their laxity clock.
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

  std::vector<PolicyBranch> branches(const TransitionSet& struct_enabled,
                                     const petri::PTPN& net,
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

  // Maximum base priority among `TaskInfo::locks` users of `lock`; std::nullopt
  // when no task uses it. Exposed for diagnostics/tests.
  [[nodiscard]] std::optional<int> lock_ceiling(const std::string& lock) const;

  // Chain positions mirrored from converter::detail::TaskChainLayout.
  static constexpr size_t kLockAcquireBase = detail::TaskLockTopology::kLockAcquireBase;
  static constexpr size_t kLockStride = detail::TaskLockTopology::kLockStride;
  static constexpr size_t kLockReleaseSuffix = detail::TaskLockTopology::kLockReleaseSuffix;

 private:
  void ensure_index(const petri::PTPN& net) const;
  const detail::TaskLockTopology::MarkingState& dynamic_state(
      const petri::PTPN& net, const petri::Marking& marking) const;

  mutable std::unique_ptr<detail::TaskLockTopology> index_;
  // One-entry memo of the marking analysis used by admissible(). The same
  // memo is shared with effective_priority() (which is static and therefore
  // does not need marking data, but must not build a second analysis).
  mutable detail::TaskLockTopology::MarkingState cached_layout_;
  mutable petri::Marking cached_marking_;
  mutable const petri::PTPN* cached_net_ = nullptr;
  mutable bool cache_valid_ = false;
};

// Policy -> rule mapping:
//   FIXED, FIXED_PRIOR_WITH_RESUME, RM, DM -> static priorities, RESUME
//   EDF                                   -> EdfFiringRule, release clocks, RESUME
//   FIFO                                  -> FifoFiringRule, release clocks,
//                                            NON_PREEMPTIVE
//   LLF                                   -> LlfFiringRule, laxity clocks,
//                                            RESUME (Event-driven LLF only)
//   PIP                                   -> PipFiringRule, RESUME
//   PCP                                   -> PcpFiringRule, RESUME
//   SRP                                   -> SrpFiringRule, RESUME
//   FIXED_PRIOR_WITH_RESTART              -> static priorities, RESTART
//   UNKNOWN                               -> static priorities, NON_PREEMPTIVE
//
// The policy-only overload cannot precompute the dynamic protocols' task-chain
// index; it returns a rule that lazily indexes `PolicyContext::net` on first
// use. Use the net-aware overload when the net is available.
std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy);
std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy, const petri::PTPN& net);

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_POLICY_H
