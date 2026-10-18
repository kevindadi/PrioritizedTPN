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

    // PIP, PCP and SRP infer lock ownership from the position of the single
    // job token in a task chain. A chain capacity above one admits queued jobs
    // and makes that position ambiguous, so the supported class is enforced
    // here for the dynamic lock protocols.
    if (configured.policy == SchedulePolicy::PIP ||
        configured.policy == SchedulePolicy::PCP ||
        configured.policy == SchedulePolicy::SRP) {
      const std::string name = schedule_policy_to_string(configured.policy);
      if (configured.task_place_capacity != 1) {
        throw std::invalid_argument(
            name + " requires task_place_capacity == 1 (one in-flight job per task), got " +
            std::to_string(configured.task_place_capacity));
      }
      for (const auto& start : configured.start_tasks) {
        if (start.tokens > 1) {
          throw std::invalid_argument(
              name + " requires at most one initial job per task; start binding for '" +
              start.task + "' requests " + std::to_string(start.tokens) + " tokens");
        }
      }
    }

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
