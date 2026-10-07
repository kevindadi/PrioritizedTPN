#ifndef LOWER_TDG2PN_VERTEX_LOWERING_H
#define LOWER_TDG2PN_VERTEX_LOWERING_H

#include "model/petri.h"
#include "model/tdg.h"

namespace converter::detail {

// Lowers every TDG node into a place/transition fragment:
//
//    task node -> a place/transition "chain" (entry -> get_core -> ready ->
//                 exec segments -> exit); N locks split the execution into
//                 2N+1 timed segments with an immediate acquire transition
//                 plus a hold place per lock
//    fork/join -> a single transition (interval/core/priority from the node)
//    empty     -> a single place
//
// node_start_end_map[name] records each fragment's entry/exit for edge
// lowering; node_pn_map[name] keeps the full task chain for resource binding.
void lower_vertices(petri::PTPN& ptpn, const tdg::TDG& tdg);

}  // namespace converter::detail

#endif  // LOWER_TDG2PN_VERTEX_LOWERING_H
