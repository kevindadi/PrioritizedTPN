#ifndef ANALYSIS_REACHABILITY_SCHEDULING_H
#define ANALYSIS_REACHABILITY_SCHEDULING_H

#include <vector>

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

  // All admissible selected-active sets E_pri-hat(M). Within each core the
  // candidates are the structurally enabled transitions at the maximal
  // priority; a real core is bounded by K = parallelism_of_core(core) (default
  // 1), so every K-subset of the candidates is a choice. For K=1 this
  // enumerates every equal-priority tie resolution. The control core (-1) is
  // unbounded and contributes the single choice containing all
  // maximal-priority transitions, because it is not CPU-scheduled.
  //
  // The result is the cartesian product over cores, sorted and duplicate-free.
  // There is deliberately no deterministic single-choice variant: fixing a tie
  // would remove reachable schedules and is not sound for worst-case analysis.
  static std::vector<TransitionSet> priority_branches(const TransitionSet& struct_enabled,
                                                      const petri::PTPN& net);
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_H
