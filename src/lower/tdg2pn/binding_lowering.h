#ifndef LOWER_TDG2PN_BINDING_LOWERING_H
#define LOWER_TDG2PN_BINDING_LOWERING_H

#include "model/petri.h"
#include "model/tdg.h"

namespace converter::detail {

// Initial tokens for `start` bindings.
void add_start_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg);

// Periodic release sub-net: (period) -> [fire(P)] -> (period), (entry).
// Skipped for tasks that already express periodic activation via a self-loop
// release edge.
void add_periodic_release_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg);

// Sink consume transitions for leaf tasks and explicit `end` tasks.
void add_end_consumers(petri::PTPN& ptpn, const tdg::TDG& tdg);

}  // namespace converter::detail

#endif  // LOWER_TDG2PN_BINDING_LOWERING_H
