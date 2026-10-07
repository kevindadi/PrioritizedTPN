#include "lower/tdg2pn/vertex_lowering.h"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "lower/tdg2pn/tdg2pn_common.h"

namespace converter::detail {

namespace {

int encode_task_execution_priority(int task_priority) {
  return task_priority;
}

std::vector<size_t> add_execution_chain(petri::PTPN& ptpn, const std::string& task_name,
                                        const std::vector<std::pair<int, int>>& times,
                                        const std::vector<std::string>& locks, int priority,
                                        int core, bool resume_mode, int task_place_capacity) {
  if (times.empty()) {
    throw std::runtime_error("Task has no execution segments: " + task_name);
  }

  // Task-chain places saturate: producing into a full place never disables the
  // producer, tokens are merged at the capacity bound (single-server semantics,
  // e.g. periodic releases arriving while a previous instance is still queued).
  const int capacity = task_place_capacity;
  const bool saturate = true;

  std::vector<size_t> chain;
  chain.reserve(times.size() * 4 + locks.size() * 2 + 3);

  const size_t entry = ptpn.add_place(task_name + "entry", capacity, saturate);
  const int encoded_priority = encode_task_execution_priority(priority);
  const size_t get_core =
      ptpn.add_transition(task_name + "get_core", immediate_interval(), encoded_priority, core,
                          /*suspendable=*/false);
  const size_t ready = ptpn.add_place(task_name + "ready", capacity, saturate);

  ptpn.set_pre_arc(entry, get_core, 1);
  ptpn.set_post_arc(get_core, ready, 1);
  chain.insert(chain.end(), {entry, get_core, ready});

  size_t current_place = ready;

  for (size_t segment_index = 0; segment_index < times.size(); ++segment_index) {
    const auto& [start, end] = times[segment_index];
    const std::string exec_name = times.size() == 1
                                      ? task_name + "exec"
                                      : task_name + "_exec_" + std::to_string(segment_index + 1);
    // In resume mode the engine models preemption by freezing the execution
    // clock, so execution segments are suspendable. Spin-lock critical sections
    // must keep the CPU, so those segments stay non-suspendable.
    const bool segment_holds_spin_lock =
        segment_index < locks.size() && locks[segment_index].find("spin") != std::string::npos;
    const bool exec_suspendable = resume_mode && !segment_holds_spin_lock;
    const size_t exec = ptpn.add_transition(exec_name, petri::TimeInterval(start, end),
                                            encoded_priority, core, exec_suspendable);

    const bool is_last_segment = segment_index + 1 == times.size();
    const std::string next_place_name =
        is_last_segment ? task_name + "exit"
                        : task_name + "_seg_" + std::to_string(segment_index + 1) + "_done";
    const size_t next_place = ptpn.add_place(next_place_name, capacity, saturate);

    ptpn.set_pre_arc(current_place, exec, 1);
    ptpn.set_post_arc(exec, next_place, 1);
    chain.insert(chain.end(), {exec, next_place});
    current_place = next_place;

    if (segment_index < locks.size()) {
      const std::string lock_name = task_name + "_lock_" + std::to_string(segment_index + 1);
      const size_t lock_transition =
          ptpn.add_transition(lock_name, immediate_interval(), encoded_priority, core,
                              /*suspendable=*/false);
      const size_t hold_place = ptpn.add_place(
          task_name + "_hold_" + std::to_string(segment_index + 1), capacity, saturate);

      ptpn.set_pre_arc(current_place, lock_transition, 1);
      ptpn.set_post_arc(lock_transition, hold_place, 1);
      chain.insert(chain.end(), {lock_transition, hold_place});
      current_place = hold_place;
    }
  }

  return chain;
}

std::pair<size_t, size_t> add_task_node(petri::PTPN& ptpn, const TaskNode& task, bool resume_mode,
                                        int task_place_capacity) {
  std::vector<size_t> chain =
      add_execution_chain(ptpn, task.name, task.time, task.lock, task.priority, task.core,
                          resume_mode, task_place_capacity);
  ptpn.node_pn_map[task.name] = chain;
  return {chain.front(), chain.back()};
}

std::pair<size_t, size_t> add_node(petri::PTPN& ptpn, const NodeType& node_type, bool resume_mode,
                                   int task_place_capacity) {
  return visit_node(node_type, [&](const auto& node) -> std::pair<size_t, size_t> {
    using Node = std::decay_t<decltype(node)>;

    if constexpr (std::is_same_v<Node, TaskNode>) {
      return add_task_node(ptpn, node, resume_mode, task_place_capacity);
    }

    if constexpr (std::is_same_v<Node, JoinTask>) {
      const petri::TimeInterval interval(node.time.first, node.time.second);
      const size_t join_trans =
          ptpn.add_transition("Join" + std::to_string(ptpn.node_index++), interval, node.priority,
                              node.core, /*suspendable=*/false);
      return {join_trans, join_trans};
    }

    if constexpr (std::is_same_v<Node, ForkTask>) {
      const petri::TimeInterval interval(node.time.first, node.time.second);
      const size_t fork_trans =
          ptpn.add_transition("Fork" + std::to_string(ptpn.node_index++), interval, node.priority,
                              node.core, /*suspendable=*/false);
      return {fork_trans, fork_trans};
    }

    const size_t empty_place = ptpn.add_place("Empty" + std::to_string(ptpn.node_index++), 1);
    return {empty_place, empty_place};
  });
}

}  // namespace

void lower_vertices(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  const bool resume_mode = is_resume_policy(tdg.policy);
  for (const auto& node_entry : tdg.nodes_type) {
    const std::string& vertex_name = node_entry.first;
    const NodeType& node_type = node_entry.second;
    spdlog::debug("[TDG2PN] Processing vertex: {}", vertex_name);

    try {
      const auto [start_idx, end_idx] =
          add_node(ptpn, node_type, resume_mode, tdg.task_place_capacity);
      ptpn.node_start_end_map[vertex_name] = {start_idx, end_idx};
    } catch (const std::exception& e) {
      spdlog::error("[TDG2PN] Failed to transform vertex {}: {}", vertex_name, e.what());
      throw;
    }
  }

  spdlog::info("[TDG2PN] Vertex transformation completed");
}

}  // namespace converter::detail
