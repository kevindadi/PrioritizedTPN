#ifndef ANALYSIS_REACHABILITY_SCHEDULING_FIRING_RULE_H
#define ANALYSIS_REACHABILITY_SCHEDULING_FIRING_RULE_H

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
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
  // Selected active set of the source state class: the transitions the
  // scheduler had running when it made this decision. Null for initial classes
  // (nothing is running yet) and for callers that do not model scheduling
  // decisions. The scalar-priority tie rule uses it to keep a running
  // transition scheduled instead of preempting it with an equal-priority peer.
  const TransitionSet* running = nullptr;
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
    return left_task == other.left_task && right_task == other.right_task && bound == other.bound &&
           domain == other.domain;
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
    const TransitionSet& control, const std::map<int, std::vector<CoreChoice>>& by_core);

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

  // Equal-priority tie handling for the scalar-priority branch enumeration
  // (Scheduling::priority_branches). The factory copies it from the net; rules
  // with their own branches() (EDF/FIFO/LLF) resolve ties through clock
  // constraints and do not use it.
  [[nodiscard]] TieMode tie_mode() const {
    return tie_;
  }

  void set_tie_mode(TieMode mode) {
    tie_ = mode;
  }

  // FIFO admission predicate, evaluated on `context.state->marking`:
  //   true    - the transition's task job is already admitted (a chain token
  //             sits past the entry place), so the job is in service;
  //   false   - the transition's task is still waiting at its entry place for
  //             its first dispatch;
  //   nullopt - the transition does not belong to a task chain (control,
  //             fork/join, hand-written nets): the rule stays neutral.
  // The FIFO tie rule prefers admitted jobs over jobs waiting for a first
  // dispatch at the same priority. The index is cached per net.
  [[nodiscard]] std::optional<bool> job_admitted(const PolicyContext& context,
                                                 size_t transition) const;

  // True when the rule depends on per-task release clocks and therefore the
  // graph must include them in the clock layout and state identity.
  virtual bool uses_release_clocks() const {
    return false;
  }

  // True when the rule depends on per-task laxity clocks (chi = -laxity).
  virtual bool uses_laxity_clocks() const {
    return false;
  }

  // True when completion edges can be observed with a release clock to obtain
  // the exact response time of the completing job (metrics only). The graph
  // then adds a scheduling-irrelevant release clock to the layout. Only the
  // scalar-priority rules declare it: EDF/FIFO/LLF select successors through
  // clock constraints on the successor zone, so their completion observation
  // is not exact and stays a DP bound.
  virtual bool supports_completion_observation() const {
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

 private:
  TieMode tie_ = TieMode::FIFO;

  // Lazily built admission index for job_admitted(): transition -> task
  // ordinal (-1 outside task chains) plus each task's entry place and chain
  // places. Rebuilt when the rule sees a different net.
  void ensure_admission_index(const petri::PTPN& net) const;

  mutable const petri::PTPN* admission_net_ = nullptr;
  mutable std::vector<int> admission_task_;
  mutable std::vector<size_t> admission_entry_;
  mutable std::vector<std::vector<size_t>> admission_places_;
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

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_FIRING_RULE_H
