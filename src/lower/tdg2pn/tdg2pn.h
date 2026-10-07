#ifndef TDG2PN_H
#define TDG2PN_H

#include "model/petri.h"
#include "model/tdg.h"

namespace converter {

// Lowers a Task Dependency Graph into a Priority Timed Petri Net in five
// ordered stages:
//
//   TDG -> [1] vertices -> [2] edges -> [3] bindings -> [4] preemption
//       -> [5] resources
//
//   [1] lower_vertices    every TDG node -> a place/transition fragment
//   [2] lower_edges       TDG edges wire the fragments together
//   [3] bindings          initial tokens, periodic releases, sink consumers
//   [4] preemption        resume: engine-level filter (no sub-net);
//                         restart: structural preempt/resume arcs
//   [5] resources         core/lock resource places, then task metadata
//
// Each stage lives in its own translation unit under src/lower/tdg2pn/.
class TDG2PN {
 public:
  static void transform(const tdg::TDG& tdg, petri::PTPN& ptpn);
};

}  // namespace converter

#endif  // TDG2PN_H
