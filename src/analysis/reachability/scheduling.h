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

  // E_pri(M): within every core group (identified by the transition's `core`
  // attribute) keep the highest-priority structurally enabled transitions.
  // If the core declares a parallelism bound K (net.parallelism_of_core),
  // at most K transitions stay active (highest priority first, ties broken by
  // transition index) so mutual exclusion / multi-core parallelism is enforced.
  // Unbounded groups (the control core -1, or nets without a parallelism model)
  // keep every transition sharing the maximal priority (ties allowed).
  static TransitionSet filter_priority_per_core(const TransitionSet& struct_enabled,
                                                const petri::PTPN& net);
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_H
