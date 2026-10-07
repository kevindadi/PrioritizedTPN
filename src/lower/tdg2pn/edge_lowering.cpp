#include "lower/tdg2pn/edge_lowering.h"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

#include "lower/tdg2pn/tdg2pn_common.h"

namespace converter::detail {

namespace {

// Trims ASCII whitespace from both ends of a string.
std::string trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// Parses a single time bound. Accepts a non-negative integer, or an unbounded
// marker (inf / +inf / ∞ / *) mapped to petri::INF. Returns false on failure.
bool parse_time_bound(const std::string& token, int& out) {
  const std::string value = trim(token);
  if (value.empty()) {
    return false;
  }
  if (value == "inf" || value == "+inf" || value == "∞" || value == "*") {
    out = petri::INF;
    return true;
  }
  try {
    size_t consumed = 0;
    const long parsed = std::stol(value, &consumed);
    if (consumed != value.size() || parsed < 0) {
      return false;
    }
    out = static_cast<int>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// Converts a TDG edge label into the firing interval of the bridge transition
// it induces. Supported forms (surrounding []/() brackets are tolerated):
//   ""        -> [0, 0]   (immediate)
//   "a"       -> [a, a]
//   "a,b"     -> [a, b]   (b may be an unbounded marker)
// Any malformed label falls back to [0, 0] with a warning.
petri::TimeInterval parse_edge_interval(const std::string& label, const std::string& source_name,
                                        const std::string& target_name) {
  std::string body = trim(label);
  if (!body.empty() && (body.front() == '[' || body.front() == '(') &&
      (body.back() == ']' || body.back() == ')')) {
    body = trim(body.substr(1, body.size() - 2));
  }
  if (body.empty()) {
    return immediate_interval();
  }

  const auto comma = body.find(',');
  int earliest = 0;
  int latest = 0;
  bool ok = false;
  if (comma == std::string::npos) {
    ok = parse_time_bound(body, earliest);
    latest = earliest;
  } else {
    ok = parse_time_bound(body.substr(0, comma), earliest) &&
         parse_time_bound(body.substr(comma + 1), latest);
  }

  if (!ok || earliest == petri::INF || (latest != petri::INF && latest < earliest)) {
    spdlog::warn("[TDG2PN] Invalid time label '{}' on edge {} -> {}; using [0, 0]", label,
                 source_name, target_name);
    return immediate_interval();
  }
  return petri::TimeInterval(earliest, latest);
}

void add_monitor(petri::PTPN& ptpn, const std::string& task_name, int task_period_time,
                 petri::PlaceId start, petri::PlaceId end) {
  const petri::PlaceId deadline = ptpn.add_place(task_name + "deadline", 1);
  const petri::PlaceId timeout = ptpn.add_place(task_name + "timeout", 1);
  const petri::PlaceId ok = ptpn.add_place(task_name + "ok", 1);
  const petri::PlaceId end_place = ptpn.add_place(task_name + "end", 1);

  const petri::TransitionId timed = add_control_transition(
      ptpn, task_name + "timed", petri::TimeInterval(task_period_time, task_period_time));
  const petri::TransitionId ending = add_control_transition(ptpn, task_name + "ending");
  const petri::TransitionId complete = add_control_transition(ptpn, task_name + "complete");
  const petri::TransitionId timeout_transition = add_control_transition(ptpn, task_name + "out");

  ptpn.set_post_arc(ending, end_place, 1);
  ptpn.set_pre_arc(end_place, complete, 1);
  ptpn.set_post_arc(complete, ok, 1);
  // Completion cancels the pending deadline: `complete` consumes the deadline
  // token, so a task that finishes in time can never reach the timeout place.
  ptpn.set_pre_arc(deadline, complete, 1);
  ptpn.set_pre_arc(deadline, timeout_transition, 1);
  ptpn.set_post_arc(timeout_transition, timeout, 1);

  ptpn.set_pre_arc(end, ending, 1);

  ptpn.set_pre_arc(start, timed, 1);
  ptpn.set_post_arc(timed, deadline, 1);
}

void handle_self_loop(petri::PTPN& ptpn, const std::string& label, const std::string& source_name) {
  const int task_period_time = std::stoi(label);
  const auto task_start_end = ptpn.node_start_end_map.find(source_name);
  if (task_start_end == ptpn.node_start_end_map.end()) {
    throw std::runtime_error("Start/end nodes not found for: " + source_name);
  }

  add_monitor(ptpn, source_name, task_period_time, task_start_end->second.first.as_place(),
              task_start_end->second.second.as_place());
}

void handle_dashed(petri::PTPN& ptpn, const std::string& source_name,
                   const std::string& target_name) {
  // Dashed edges are handled by periodic release bindings; no direct arc is
  // added.
  (void)ptpn;
  (void)source_name;
  (void)target_name;
}

void handle_normal(petri::PTPN& ptpn, const tdg::TDG& tdg, const std::string& source_name,
                   const std::string& target_name, const std::string& label) {
  const auto source_it = ptpn.node_start_end_map.find(source_name);
  const auto target_it = ptpn.node_start_end_map.find(target_name);

  if (source_it == ptpn.node_start_end_map.end() || target_it == ptpn.node_start_end_map.end()) {
    throw std::runtime_error("Node mapping not found for edge: " + source_name + " -> " +
                             target_name);
  }

  const auto source_type_it = tdg.nodes_type.find(source_name);
  const auto target_type_it = tdg.nodes_type.find(target_name);
  if (source_type_it == tdg.nodes_type.end() || target_type_it == tdg.nodes_type.end()) {
    throw std::runtime_error("Node type not found for edge: " + source_name + " -> " + target_name);
  }

  const bool source_is_control = is_fork_or_join(source_type_it->second);
  const bool target_is_control = is_fork_or_join(target_type_it->second);

  const petri::NodeRef source_exit = source_it->second.second;
  const petri::NodeRef target_entry = target_it->second.first;

  if (source_is_control && target_is_control) {
    throw std::runtime_error("Invalid TDG edge between transition nodes: " + source_name + " -> " +
                             target_name);
  }

  // NodeRef::as_place()/as_transition() throw when the fragment kind does not
  // match the edge direction, so an inconsistent lowering fails loudly instead
  // of silently writing into the wrong matrix.
  if (source_is_control) {
    ptpn.set_post_arc(source_exit.as_transition(), target_entry.as_place(), 1);
    return;
  }

  if (target_is_control) {
    ptpn.set_pre_arc(source_exit.as_place(), target_entry.as_transition(), 1);
    return;
  }

  // A task -> task edge is realised by a dedicated bridge transition, whose
  // firing interval comes from the edge label (fork/join-adjacent edges wire
  // directly into the fork/join transition and ignore the label).
  const petri::TimeInterval interval = parse_edge_interval(label, source_name, target_name);
  const petri::TransitionId bridge_transition =
      add_control_transition(ptpn, source_name + "_to_" + target_name, interval);

  ptpn.set_pre_arc(source_exit.as_place(), bridge_transition, 1);
  ptpn.set_post_arc(bridge_transition, target_entry.as_place(), 1);

  spdlog::debug("[TDG2PN] Added edge: {} -> {}", source_name, target_name);
}

}  // namespace

void lower_edges(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  for (const auto& edge : tdg.tdg_edges) {
    try {
      if (edge.is_self_loop()) {
        handle_self_loop(ptpn, edge.label, edge.source);
        continue;
      }

      if (edge.is_dashed()) {
        handle_dashed(ptpn, edge.source, edge.target);
        continue;
      }

      handle_normal(ptpn, tdg, edge.source, edge.target, edge.label);
    } catch (const std::exception& exception) {
      spdlog::error("[TDG2PN] Failed to transform edge: {}", exception.what());
      throw;
    }
  }
}

}  // namespace converter::detail
