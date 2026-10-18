#include "lower/tdg2pn/binding_lowering.h"

#include <algorithm>
#include <set>
#include <spdlog/spdlog.h>
#include <string>

#include "lower/tdg2pn/tdg2pn_common.h"

namespace converter::detail {

namespace {

bool has_non_self_successor(const tdg::TDG& tdg, const std::string& task_name) {
  return std::any_of(tdg.tdg_edges.begin(), tdg.tdg_edges.end(),
                     [&](const TdgEdge& edge) { return edge.leaves(task_name); });
}

void add_consume_transition(petri::PTPN& ptpn, const std::string& task_name,
                            petri::NodeRef end_ref) {
  const petri::TransitionId consume_trans = add_control_transition(ptpn, task_name + "_consume");
  ptpn.set_pre_arc(end_ref.as_place(), consume_trans, 1);
}

}  // namespace

void add_start_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  for (const auto& start_binding : tdg.start_tasks) {
    const auto node_it = ptpn.node_start_end_map().find(start_binding.task);
    if (node_it == ptpn.node_start_end_map().end()) {
      spdlog::warn("[TDG2PN] Start task not found in node map: {}", start_binding.task);
      continue;
    }
    if (start_binding.tokens <= 0) {
      continue;
    }
    ptpn.set_initial_marking(node_it->second.first.as_place(), start_binding.tokens);
  }
}

void add_end_consumers(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  std::set<std::string> consume_tasks;

  for (const auto& [vertex_name, node_type] : tdg.nodes_type) {
    if (as_task_node(node_type) && !has_non_self_successor(tdg, vertex_name)) {
      consume_tasks.insert(vertex_name);
    }
  }

  consume_tasks.insert(tdg.end_tasks.begin(), tdg.end_tasks.end());

  for (const auto& task_name : consume_tasks) {
    const auto node_it = ptpn.node_start_end_map().find(task_name);
    if (node_it == ptpn.node_start_end_map().end()) {
      spdlog::warn("[TDG2PN] End task not found in node map: {}", task_name);
      continue;
    }
    add_consume_transition(ptpn, task_name, node_it->second.second);
  }
}

void add_periodic_release_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  for (const auto& periodic_task : tdg.periodic_tasks) {
    const auto node_it = ptpn.node_start_end_map().find(periodic_task.task);
    const auto type_it = tdg.nodes_type.find(periodic_task.task);
    if (node_it == ptpn.node_start_end_map().end() || type_it == tdg.nodes_type.end()) {
      spdlog::warn("[TDG2PN] Periodic task not found for release binding: {}", periodic_task.task);
      continue;
    }

    if (!as_task_node(type_it->second)) {
      spdlog::warn("[TDG2PN] Periodic release requested for non-task node: {}", periodic_task.task);
      continue;
    }

    const petri::PlaceId period_place =
        ptpn.add_place(periodic_task.task + "_period", 1, /*saturate=*/true);
    const petri::TransitionId fire =
        add_control_transition(ptpn, periodic_task.task + "_fire",
                               petri::TimeInterval(periodic_task.period, periodic_task.period));

    ptpn.set_initial_marking(period_place, 1);
    ptpn.set_pre_arc(period_place, fire, 1);
    ptpn.set_post_arc(fire, period_place, 1);
    ptpn.set_post_arc(fire, node_it->second.first.as_place(), 1);
  }
}

}  // namespace converter::detail
