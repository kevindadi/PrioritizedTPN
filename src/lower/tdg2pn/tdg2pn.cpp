#include "lower/tdg2pn/tdg2pn.h"

#include <exception>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

#include "lower/tdg2pn/binding_lowering.h"
#include "lower/tdg2pn/edge_lowering.h"
#include "lower/tdg2pn/policy_priorities.h"
#include "lower/tdg2pn/preemption.h"
#include "lower/tdg2pn/resource_lowering.h"
#include "lower/tdg2pn/task_metadata.h"
#include "lower/tdg2pn/vertex_lowering.h"

namespace converter {

void TDG2PN::transform(const tdg::TDG& tdg, petri::PTPN& ptpn) {
  try {
    spdlog::info("[TDG2PN] Starting TDG to PTPN transformation");

    // Lower a local copy so RM/DM can rewrite task priorities (the public
    // signature stays const and the caller's TDG is never mutated).
    tdg::TDG configured = tdg;
    detail::apply_policy_priorities(configured);

    ptpn.clear_lowering_metadata();
    ptpn.set_schedule_policy(configured.policy);

    spdlog::info("[TDG2PN] Transforming vertices");
    detail::lower_vertices(ptpn, configured);

    spdlog::info("[TDG2PN] Transforming edges");
    detail::lower_edges(ptpn, configured);

    detail::add_start_bindings(ptpn, configured);
    detail::add_periodic_release_bindings(ptpn, configured);
    detail::add_end_consumers(ptpn, configured);

    detail::make_preemption_strategy(configured.policy)->apply(ptpn, configured);

    spdlog::info("[TDG2PN] Adding resources and bindings");
    detail::add_resources_and_bindings(ptpn, configured);

    detail::populate_task_info(ptpn, configured);

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
