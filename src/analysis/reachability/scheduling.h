#ifndef ANALYSIS_REACHABILITY_SCHEDULING_H
#define ANALYSIS_REACHABILITY_SCHEDULING_H

#include <vector>

#include "analysis/reachability/scheduling/firing_rule.h"
#include "analysis/reachability/state_class.h"
#include "model/petri.h"

namespace state_class {

// Structural and priority enabling, the two set operations the state-class
// construction relies on. Priority filtering is done per core, matching the
// fixed-priority scheduling semantics of the formal model.
class Scheduling {
 public:
  // E_struct(M): transitions whose input places hold enough tokens. Sorted.
  static TransitionSet structural_enabled(const petri::PTPN& net, const petri::Marking& marking);

  // All admissible selected-active sets E_pri-hat(M). The firing rule first
  // filters `struct_enabled` through admissible(); the survivors are grouped by
  // core and ranked by effective_priority(). Within each core the candidates
  // are the admissible transitions at the maximal priority; a real core is
  // bounded by K = parallelism_of_core(core) (default 1), so every K-subset of
  // the candidates is a choice. For K=1 this enumerates every equal-priority
  // tie resolution. The control core (-1) is unbounded and contributes the
  // single choice containing all maximal-priority transitions, because it is
  // not CPU-scheduled. A core with no admissible transition contributes no
  // choice; if every core is empty the result is one empty active set.
  //
  // Equal-priority ties are resolved by `rule.tie_mode()`:
  //  - TieMode::FIFO (default): a transition that `context.running` reports as
  //    already running keeps its slot when it is among the maximal-priority
  //    candidates; equal-priority peers cannot preempt it. Additionally, at
  //    the cutoff priority, jobs whose chain token has left the entry place
  //    (`FiringRule::job_admitted`) take the remaining slots before jobs
  //    waiting for a first dispatch, so a task stays in service across its
  //    lock/segment boundaries. With K = 1 this yields exactly one choice per
  //    core. Initial classes (nothing running, every job at its entry) still
  //    enumerate every tie, because the first dispatch has no winner yet.
  //  - TieMode::ARBITRARY: every equal-priority choice is enumerated, including
  //    preemption by a peer (worst case over all tie-breaks).
  //
  // The result is the cartesian product over cores, sorted and duplicate-free.
  static std::vector<TransitionSet> priority_branches(const TransitionSet& struct_enabled,
                                                      const petri::PTPN& net,
                                                      const FiringRule& rule,
                                                      const PolicyContext& context);
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_H
