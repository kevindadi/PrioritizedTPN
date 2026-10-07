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

    ptpn.clear_lowering_metadata();

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
                 ptpn.num_places(), ptpn.num_transitions());

    if (!ptpn.verify_structure()) {
      spdlog::warn("[TDG2PN] Structure verification failed, continuing anyway");
    }
  } catch (const std::exception& e) {
    spdlog::error("[TDG2PN] Failed to transform TDG to PTPN: {}", e.what());
    throw;
  }
}

}  // namespace converter
