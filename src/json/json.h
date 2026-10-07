#ifndef JSON_TDG_PARSER_H
#define JSON_TDG_PARSER_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "types/types.h"

namespace parse {

struct ParseResult {
  bool success;
  std::string error_message;
  int error_line = 0;
};

struct ValidationResult {
  bool success = true;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  void add_error(const std::string& err) {
    success = false;
    errors.push_back(err);
  }

  void add_warning(const std::string& warn) {
    warnings.push_back(warn);
  }
};

struct JsonNode {
  std::string id;
  std::string type;
  int priority = 100;
  int core = 0;
  bool has_priority = false;  // whether "priority" was present in the JSON
  bool has_core = false;      // whether "core" was present in the JSON
  std::vector<std::pair<int, int>> time;
  std::vector<std::string> locks;

  NodeType to_node_type() const;
};

struct JsonEdge {
  std::string source;
  std::string target;
  std::string label;
  std::string style;
};

struct JsonGraph {
  std::string name = "G";
  int num_cpus = 1;
  int cores_per_cpu = 1;
  // Capacity of every task-chain place (entry/ready/segment/...). Overflowing
  // tokens saturate at this bound instead of disabling the producing transition.
  int task_place_capacity = 1;
  std::vector<std::string> shared_locks;
  SchedulePolicy policy = SchedulePolicy::FIXED;  // 调度策略
  std::vector<StartBinding> start_tasks;
  std::vector<std::string> end_tasks;
  std::vector<PeriodicBinding> periodic_tasks;
  std::vector<JsonNode> nodes;
  std::vector<JsonEdge> edges;
};

class Parser {
 public:
  Parser() = default;

  ParseResult parse_file(const std::string& file_path);
  ParseResult parse_string(const std::string& json_content);

  std::string get_graph_name() const {
    return graph_.name;
  }

  int get_num_cpus() const {
    return graph_.num_cpus;
  }

  int get_cores_per_cpu() const {
    return graph_.cores_per_cpu;
  }

  int get_task_place_capacity() const {
    return graph_.task_place_capacity;
  }

  SchedulePolicy get_policy() const {
    return graph_.policy;
  }

  const std::vector<StartBinding>& get_start_tasks() const {
    return graph_.start_tasks;
  }

  const std::vector<std::string>& get_end_tasks() const {
    return graph_.end_tasks;
  }

  const std::vector<PeriodicBinding>& get_periodic_tasks() const {
    return graph_.periodic_tasks;
  }

  const std::vector<JsonNode>& get_nodes() const {
    return graph_.nodes;
  }

  const std::vector<JsonEdge>& get_edges() const {
    return graph_.edges;
  }

  ValidationResult validate() const;
  std::string to_dot_string() const;

 private:
  void parse_graph_object(const nlohmann::json& graph_obj);
  void parse_configuration_object(const nlohmann::json& config);
  void parse_nodes_array(const nlohmann::json& nodes_array);
  void parse_edges_array(const nlohmann::json& edges_array);
  JsonNode parse_node_object(const nlohmann::json& node_obj);

  JsonGraph graph_;
  std::string original_json_;
};

std::string node_to_dot_label(const NodeType& node);

enum class LockType { MUTEX, SPIN, UNKNOWN };
LockType get_lock_type(const std::string& lock_name);
std::string get_lock_type_short(const std::string& lock_name);
std::string format_locks_with_type(const std::vector<std::string>& locks);

// 时间区间计算
int calculate_time_interval_count(int lock_count);
std::string get_time_interval_label(int index, const std::vector<std::string>& locks);

}  // namespace parse

#endif  // JSON_TDG_PARSER_H