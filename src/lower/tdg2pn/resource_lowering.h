#ifndef LOWER_TDG2PN_RESOURCE_LOWERING_H
#define LOWER_TDG2PN_RESOURCE_LOWERING_H

#include "model/petri.h"
#include "model/tdg.h"

namespace converter::detail {

// Adds core/lock resource places and binds the task chains to them.
//
// The resume policy expresses per-core mutual exclusion and preemption purely
// through the analysis engine's per-core priority filter, so it omits the
// structural CPU-resource place and records each CPU's parallelism instead.
// Other policies (restart / PToPNer export) keep the place-based encoding.
void add_resources_and_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg);

}  // namespace converter::detail

#endif  // LOWER_TDG2PN_RESOURCE_LOWERING_H
