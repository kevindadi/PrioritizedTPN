#ifndef TDG2PN_H
#define TDG2PN_H

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "petri/petri.h"
#include "tdg/tdg.h"
#include "types/types.h"

namespace converter {

class TDG2PN {
 public:
  static void transform(const tdg::TDG& tdg, petri::PTPN& ptpn);

 private:
  static std::unordered_map<int, std::vector<std::string>> classify_tdg_priority(
      const tdg::TDG& tdg);
  static std::unordered_map<std::string, int> build_preempt_priorities(
      const std::vector<std::string>& tasks, const std::unordered_map<std::string, TaskConfig>& tc,
      int aggressor_priority);
  static void transform_vertices(petri::PTPN& ptpn, const tdg::TDG& tdg);
  static void transform_edges(petri::PTPN& ptpn, const tdg::TDG& tdg);
  static std::pair<size_t, size_t> add_node_matrix(petri::PTPN& ptpn, const NodeType& node_type,
                                                   bool resume_mode, int task_place_capacity);
  static std::pair<size_t, size_t> add_task_node_matrix(petri::PTPN& ptpn, const TaskNode& task,
                                                        bool resume_mode, int task_place_capacity);
  static std::vector<size_t> add_execution_chain(petri::PTPN& ptpn, const std::string& task_name,
                                                 const std::vector<std::pair<int, int>>& times,
                                                 const std::vector<std::string>& locks,
                                                 int priority, int core, bool resume_mode,
                                                 int task_place_capacity);
  static void add_monitor_matrix(petri::PTPN& ptpn, const std::string& task_name,
                                 int task_period_time, size_t start, size_t end);
  static void add_start_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg);
  static void add_end_consumers(petri::PTPN& ptpn, const tdg::TDG& tdg);
  static void add_periodic_release_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg);
  static void add_consume_transition(petri::PTPN& ptpn, const std::string& task_name,
                                     size_t end_idx);
  static bool has_non_self_successor(const tdg::TDG& tdg, const std::string& task_name);
  static bool has_self_loop_release(const tdg::TDG& tdg, const std::string& task_name);
  static void fixed_prior_with_restart(
      petri::PTPN& ptpn, const std::unordered_map<int, std::vector<std::string>>& core_task,
      const std::unordered_map<std::string, TaskConfig>& tc,
      const std::unordered_map<std::string, NodeType>& nodes_type);
  static void add_resources_and_bindings_matrix(petri::PTPN& ptpn, const tdg::TDG& tdg);
  static void populate_task_info(petri::PTPN& ptpn, const tdg::TDG& tdg);
  static void add_cpu_resource_matrix(petri::PTPN& ptpn, int cpus, int cores_per_cpu);
  static void add_lock_resource_matrix(petri::PTPN& ptpn, const std::set<std::string>& locks_name);
  static void task_bind_cpu_resource_matrix(petri::PTPN& ptpn,
                                            const std::vector<NodeType>& all_task);
  static void task_bind_lock_resource_matrix(
      petri::PTPN& ptpn, const std::vector<NodeType>& all_task,
      const std::map<std::string, std::vector<std::string>>& task_locks);
  static void bind_task_locks_matrix(
      petri::PTPN& ptpn, const std::string& task_name, const std::vector<std::string>& lock_types,
      const std::vector<size_t>& task_pt_chain,
      const std::map<std::string, std::vector<std::string>>& task_locks);
  static void handle_self_loop_edge_matrix(petri::PTPN& ptpn, const std::string& label,
                                           const std::string& source_name);
  static void handle_dashed_edge_matrix(petri::PTPN& ptpn, const std::string& source_name,
                                        const std::string& target_name);
  static void handle_normal_edge_matrix(petri::PTPN& ptpn, const tdg::TDG& tdg,
                                        const std::string& source_name,
                                        const std::string& target_name, const std::string& label);
};

}  // namespace converter

#endif  // TDG2PN_H