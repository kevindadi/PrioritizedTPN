#ifndef TDG_H
#define TDG_H

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "model/types.h"

namespace parse {
class Parser;
}

namespace tdg {

// WCET provenance for a task. `kind` is the trust class ("model",
// "certified", "measured", or "estimated"); `method` and `artifact` are
// free-form provenance such as the analysis tool and the measured binary.
struct WcetProvenance {
  std::string kind = "model";
  std::string method;
  std::string artifact;
};

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
  // Per-task core-allocation domain: the explicit `allowed_cores` list, or
  // the singleton {core} when the input omits it.
  std::unordered_map<std::string, std::vector<int>> task_allowed_cores;
  // Per-task WCET provenance (trust class plus optional method/artifact).
  std::unordered_map<std::string, WcetProvenance> task_wcet_provenance;
  std::vector<TdgEdge> tdg_edges;

  void parse_json(const std::string& json_file);
  void parse_json_string(const std::string& json_content);
  // Populates the TDG from an already-parsed JSON graph, avoiding a second read.
  void load_from_parser(const parse::Parser& parser, bool log_nodes = false);
};

}  // namespace tdg

#endif  // TDG_H