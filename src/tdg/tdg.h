#ifndef TDG_H
#define TDG_H

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "types/types.h"

namespace parse {
class Parser;
}

namespace tdg {

class TDG {
 public:
  TDG() = default;

  TDG(int num_cpus, int cores_per_cpu) : num_cpus(num_cpus), cores_per_cpu(cores_per_cpu) {}

  int num_cpus = 1;
  int cores_per_cpu = 1;
  // Capacity of every task-chain place; overflow saturates instead of
  // disabling the producing transition (see docs/json_format.md).
  int task_place_capacity = 1;
  SchedulePolicy policy = SchedulePolicy::FIXED;

  std::vector<NodeType> all_task;
  std::vector<StartBinding> start_tasks;
  std::vector<std::string> end_tasks;
  std::vector<PeriodicBinding> periodic_tasks;
  std::unordered_map<std::string, int> tasks_priority;
  std::unordered_map<std::string, NodeType> nodes_type;
  std::set<std::string> lock_set;
  std::map<std::string, std::vector<std::string>> task_locks_map;
  std::vector<TdgEdge> tdg_edges;

  void parse_json(const std::string& json_file);
  void parse_json_string(const std::string& json_content);
  // Populates the TDG from an already-parsed JSON graph, avoiding a second read.
  void load_from_parser(const parse::Parser& parser, bool log_nodes = false);
  void export_to_dot(const std::string& output_path);
  std::string to_dot_string() const;
};

}  // namespace tdg

#endif  // TDG_H