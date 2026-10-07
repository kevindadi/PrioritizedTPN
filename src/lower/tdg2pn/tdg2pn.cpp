#include "lower/tdg2pn/tdg2pn.h"

#include <exception>
#include <spdlog/spdlog.h>

#include "lower/tdg2pn/binding_lowering.h"
#include "lower/tdg2pn/edge_lowering.h"
#include "lower/tdg2pn/preemption.h"
#include "lower/tdg2pn/resource_lowering.h"
#include "lower/tdg2pn/task_metadata.h"
#include "lower/tdg2pn/vertex_lowering.h"

namespace converter {

void TDG2PN::transform(const tdg::TDG& tdg, petri::PTPN& ptpn) {
  try {
    spdlog::info("[TDG2PN] Starting TDG to PTPN transformation");

    ptpn.node_start_end_map.clear();
    ptpn.node_pn_map.clear();
    ptpn.cpus_place.clear();
    ptpn.core_parallelism.clear();
    ptpn.locks_place.clear();
    ptpn.task_info.clear();
    ptpn.node_index = 0;

    spdlog::info("[TDG2PN] Transforming vertices");
    detail::lower_vertices(ptpn, tdg);

    spdlog::info("[TDG2PN] Transforming edges");
    detail::lower_edges(ptpn, tdg);

    detail::add_start_bindings(ptpn, tdg);
    detail::add_periodic_release_bindings(ptpn, tdg);
    detail::add_end_consumers(ptpn, tdg);

    detail::make_preemption_strategy(tdg.policy)->apply(ptpn, tdg);

    spdlog::info("[TDG2PN] Adding resources and bindings");
    detail::add_resources_and_bindings(ptpn, tdg);

    detail::populate_task_info(ptpn, tdg);

    spdlog::info("[TDG2PN] TDG transformation completed: {} places, {} transitions",
                 ptpn.places.size(), ptpn.transitions.size());

    if (!ptpn.verify_structure()) {
      spdlog::warn("[TDG2PN] Structure verification failed, continuing anyway");
    }
  } catch (const std::exception& e) {
    spdlog::error("[TDG2PN] Failed to transform TDG to PTPN: {}", e.what());
    throw;
  }
}

}  // namespace converter
