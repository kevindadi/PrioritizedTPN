#ifndef LOWER_TDG2PN_TASK_METADATA_H
#define LOWER_TDG2PN_TASK_METADATA_H

#include "model/petri.h"
#include "model/tdg.h"

namespace converter::detail {

// Fills ptpn.task_info (WCET/BCET, period, deadline, locks) so the metrics
// layer can reason about deadlines/periods without re-reading the TDG.
void populate_task_info(petri::PTPN& ptpn, const tdg::TDG& tdg);

}  // namespace converter::detail

#endif  // LOWER_TDG2PN_TASK_METADATA_H
