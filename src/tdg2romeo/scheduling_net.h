#ifndef TDG2ROMEO_SCHEDULING_NET_H
#define TDG2ROMEO_SCHEDULING_NET_H

#include "tdg/tdg.h"
#include "tdg2romeo.h"

namespace romeo {

RomeoModel build_scheduling_net_model(const tdg::TDG& tdg, const RomeoExportOptions& opts);

}  // namespace romeo

#endif  // TDG2ROMEO_SCHEDULING_NET_H
