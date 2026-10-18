#ifndef LOWER_TDG2PN_EDGE_LOWERING_H
#define LOWER_TDG2PN_EDGE_LOWERING_H

#include "model/petri.h"
#include "model/tdg.h"

namespace converter::detail {

// Wires the vertex fragments together following the TDG edges:
//
//    task -> task        bridge transition, interval parsed from the edge label
//    task -> fork/join   direct arc into the fork/join transition (label ignored)
//    fork/join -> task   direct arc from the fork/join transition (label ignored)
//    self-loop           ignored (allowed annotation, no monitor sub-net)
//    dashed style        periodic release binding (handled by binding lowering)
void lower_edges(petri::PTPN& ptpn, const tdg::TDG& tdg);

}  // namespace converter::detail

#endif  // LOWER_TDG2PN_EDGE_LOWERING_H
