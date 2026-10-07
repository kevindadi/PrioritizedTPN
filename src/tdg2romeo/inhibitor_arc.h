#ifndef TDG2ROMEO_INHIBITOR_ARC_H
#define TDG2ROMEO_INHIBITOR_ARC_H

#include "tdg/tdg.h"
#include "tdg2romeo.h"

namespace romeo {

RomeoModel build_inhibitor_arc_model(const tdg::TDG& tdg, const RomeoExportOptions& opts);

}  // namespace romeo

#endif  // TDG2ROMEO_INHIBITOR_ARC_H
