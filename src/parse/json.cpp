#include "parse/json.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string_view>
#include <unordered_map>

using nlohmann::json;

namespace {

constexpr const char* kNodeTypeTask = "task";
constexpr const char* kNodeTypeFork = "fork";
constexpr const char* kNodeTypeJoin = "join";
constexpr const char* kNodeTypeEmpty = "empty";

// Joins values with a delimiter; returns fallback when the range is empty.
template <typename Range, typename Formatter>
std::string join(const Range& values, std::string_view delimiter, Formatter formatter,
                 std::string_view fallback = "none") {
  if (values.empty()) {
    return std::string(fallback);
  }

  std::ostringstream oss;
  bool first = true;
  for (const auto& value : values) {
    if (!first) {
      oss << delimiter;
    }
    first = false;
    oss << formatter(value);
  }
  return oss.str();
}

std::string format_range(const std::pair<int, int>& range) {
  return "[" + std::to_string(range.first) + ", " + std::to_string(range.second) + "]";
}

std::string format_time_ranges(const std::vector<std::pair<int, int>>& time_ranges) {
  return join(time_ranges, ", ", format_range);
}

std::string format_locks(const std::vector<std::string>& locks) {
  return join(locks, ", ", [](const std::string& lock) { return lock; });
}

StartBinding parse_start_binding(const json& binding_obj) {
  StartBinding binding;
  if (binding_obj.is_string()) {
    binding.task = binding_obj.get<std::string>();
    return binding;
  }

  binding.task = binding_obj.value("task", "");
  if (binding_obj.contains("tokens")) {
    binding.tokens = binding_obj["tokens"].get<int>();
  }
  return binding;
}

PeriodicBinding parse_periodic_binding(const json& binding_obj) {
  PeriodicBinding binding;
  binding.task = binding_obj.value("task", "");
  if (binding_obj.contains("period")) {
    binding.period = binding_obj["period"].get<int>();
  }
  return binding;
}

bool is_task_type(const std::string& type) {
  return type == kNodeTypeTask;
}

// Returns true when any edge matches the given predicate.
template <typename Predicate>
bool any_edge(const parse::JsonGraph& graph, Predicate predicate) {
  return std::any_of(graph.edges.begin(), graph.edges.end(), predicate);
}

bool has_incoming_edge(const parse::JsonGraph& graph, const std::string& node_id) {
  return any_edge(graph, [&](const parse::JsonEdge& edge) {
    return edge.target == node_id && edge.source != node_id;
  });
}

bool has_outgoing_edge(const parse::JsonGraph& graph, const std::string& node_id) {
  return any_edge(graph, [&](const parse::JsonEdge& edge) {
    return edge.source == node_id && edge.target != node_id;
  });
}

bool has_self_loop_edge(const parse::JsonGraph& graph, const std::string& node_id) {
  return any_edge(graph, [&](const parse::JsonEdge& edge) {
    return edge.source == node_id && edge.target == node_id;
  });
}

std::string build_node_label(const parse::JsonNode& node) {
  std::ostringstream oss;
  oss << node.id << "\\n" << node.type;

  if (node.type == kNodeTypeTask) {
    oss << "\\nprio=" << node.priority << " core=" << node.core;
    if (!node.time.empty()) {
      oss << "\\ntime=" << format_time_ranges(node.time);
    }
    oss << "\\nlocks=" << format_locks(node.locks);
  }

  return oss.str();
}

// Parses a single [min, max] time interval from a JSON array element.
std::optional<std::pair<int, int>> parse_time_interval(const json& time_range) {
  if (!time_range.is_array() || time_range.size() != 2) {
    return std::nullopt;
  }
  return std::make_pair(time_range[0].get<int>(), time_range[1].get<int>());
}

}  // namespace

namespace parse {

LockType get_lock_type(const std::string& lock_name) {
  if (lock_name.rfind("mutex", 0) == 0) {
    return LockType::MUTEX;
  }
  if (lock_name.rfind("spin", 0) == 0) {
    return LockType::SPIN;
  }
  return LockType::UNKNOWN;
}

std::string get_lock_type_short(const std::string& lock_name) {
  switch (get_lock_type(lock_name)) {
    case LockType::MUTEX:
      return "[M]";
    case LockType::SPIN:
      return "[S]";
    default:
      return "[?]";
  }
}

std::string format_locks_with_type(const std::vector<std::string>& locks) {
  if (locks.empty()) {
    return "none";
  }

  return join(locks, " ",
              [&](const std::string& lock) { return get_lock_type_short(lock) + lock; });
}

// Each lock adds a pre-CS, CS, and post-CS segment: total = 2 * locks + 1.
int calculate_time_interval_count(int lock_count) {
  return 2 * lock_count + 1;
}

std::string get_time_interval_label(int index, const std::vector<std::string>& locks) {
  const int lock_count = static_cast<int>(locks.size());

  if (lock_count == 0) {
    return "[Exec]";
  }

  if (lock_count == 1) {
    switch (index) {
      case 0:
        return "[Pre]";
      case 1:
        return "[CS:" + locks[0] + "]";
      case 2:
        return "[Post]";
      default:
        return "[?]";
    }
  }

  // Nested locking: lock1 -> lock2 -> ... -> lockN -> unlockN -> ... ->
  // unlock1.
  if (index < lock_count) {
    return index == 0 ? "[Pre:" + locks[0] + "]" : "[CS:" + locks[index - 1] + "]";
  }
  if (index == lock_count) {
    return "[CS:" + locks[lock_count - 1] + "]";
  }

  const int post_index = index - lock_count;
  return post_index == 0 ? "[Post:" + locks[lock_count - 1] + "]"
                         : "[Post" + std::to_string(post_index) + "]";
}

ParseResult Parser::parse_file(const std::string& file_path) {
  spdlog::info("[JSON] Starting JSON parsing: {}", file_path);

  std::ifstream file(file_path);
  if (!file.is_open()) {
    spdlog::error("[JSON] Failed to open file: {}", file_path);
    return {false, "Failed to open file: " + file_path, 0};
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  original_json_ = buffer.str();

  return parse_string(original_json_);
}

ParseResult Parser::parse_string(const std::string& json_content) {
  spdlog::info("[JSON] Parsing JSON content ({} characters)", json_content.size());

  graph_ = JsonGraph{};
  original_json_ = json_content;

  try {
    const auto document = json::parse(json_content);
    spdlog::debug("[JSON] JSON parsed successfully");

    if (document.contains("graph")) {
      parse_graph_object(document["graph"]);
    }
    if (document.contains("configuration")) {
      parse_configuration_object(document["configuration"]);
    }
    if (document.contains("nodes")) {
      parse_nodes_array(document["nodes"]);
    }
    if (document.contains("edges")) {
      parse_edges_array(document["edges"]);
    }

    spdlog::info("[JSON] Graph name: {}", graph_.name);
    spdlog::info("[JSON] Configuration: {} CPUs, {} cores per CPU", graph_.num_cpus,
                 graph_.cores_per_cpu);
    spdlog::info("[JSON] Parsed {} nodes, {} edges", graph_.nodes.size(), graph_.edges.size());
    spdlog::info("[JSON] JSON parsing completed successfully");
    return {true, "", 0};

  } catch (const json::parse_error& e) {
    spdlog::error("[JSON] Parse error: {}", e.what());
    return {false, e.what(), static_cast<int>(e.byte)};
  } catch (const std::exception& e) {
    spdlog::error("[JSON] Error: {}", e.what());
    return {false, e.what(), 0};
  }
}

void Parser::parse_graph_object(const json& graph_obj) {
  graph_.name = graph_obj.value("name", graph_.name);
}

void Parser::parse_configuration_object(const json& config) {
  graph_.num_cpus = config.value("num_cpus", graph_.num_cpus);
  graph_.cores_per_cpu = config.value("cores_per_cpu", graph_.cores_per_cpu);
  graph_.task_place_capacity = config.value("task_place_capacity", graph_.task_place_capacity);

  if (config.contains("shared_locks")) {
    graph_.shared_locks = config["shared_locks"].get<std::vector<std::string>>();
  }
  if (config.contains("policy")) {
    graph_.policy = parse_schedule_policy(config["policy"].get<std::string>());
  }
  if (config.contains("start")) {
    for (const auto& start_obj : config["start"]) {
      graph_.start_tasks.push_back(parse_start_binding(start_obj));
    }
  }
  if (config.contains("end")) {
    graph_.end_tasks = config["end"].get<std::vector<std::string>>();
  }
  if (config.contains("periodic")) {
    for (const auto& periodic_obj : config["periodic"]) {
      graph_.periodic_tasks.push_back(parse_periodic_binding(periodic_obj));
    }
  }
}

void Parser::parse_nodes_array(const json& nodes_array) {
  graph_.nodes.reserve(nodes_array.size());
  for (const auto& node_obj : nodes_array) {
    graph_.nodes.push_back(parse_node_object(node_obj));
  }
}

void Parser::parse_edges_array(const json& edges_array) {
  graph_.edges.reserve(edges_array.size());
  for (const auto& edge_obj : edges_array) {
    JsonEdge edge;
    edge.source = edge_obj.value("source", "");
    edge.target = edge_obj.value("target", "");
    edge.label = edge_obj.value("label", "");
    edge.style = edge_obj.value("style", "");
    graph_.edges.push_back(std::move(edge));
  }
}

JsonNode Parser::parse_node_object(const json& node_obj) {
  JsonNode node;
  node.id = node_obj.value("id", "");
  node.type = node_obj.value("type", "");
  node.has_priority = node_obj.contains("priority");
  node.has_core = node_obj.contains("core");
  node.priority = node_obj.value("priority", node.priority);
  node.core = node_obj.value("core", node.core);

  if (node_obj.contains("time")) {
    for (const auto& time_range : node_obj["time"]) {
      if (auto interval = parse_time_interval(time_range)) {
        node.time.push_back(*interval);
      }
    }
  }

  if (node_obj.contains("locks")) {
    node.locks = node_obj["locks"].get<std::vector<std::string>>();
  }

  return node;
}

ValidationResult Parser::validate() const {
  ValidationResult result;

  if (graph_.task_place_capacity < 1) {
    result.add_error("task_place_capacity must be >= 1, got " +
                     std::to_string(graph_.task_place_capacity));
  }

  const std::set<std::string> valid_types = {kNodeTypeTask, kNodeTypeFork, kNodeTypeJoin,
                                             kNodeTypeEmpty};
  const std::set<std::string> defined_locks(graph_.shared_locks.begin(), graph_.shared_locks.end());
  const int max_core = graph_.num_cpus * graph_.cores_per_cpu - 1;

  std::set<std::string> node_ids;
  std::unordered_map<std::string, std::string> node_types;

  for (const auto& node : graph_.nodes) {
    if (!node_ids.insert(node.id).second) {
      result.add_error("Duplicate node ID: " + node.id);
    }
    node_types[node.id] = node.type;

    if (valid_types.count(node.type) == 0) {
      result.add_error("Unknown node type: " + node.type + " for node " + node.id);
    }

    if (node.type == kNodeTypeTask && (node.core < 0 || node.core > max_core)) {
      result.add_error("Invalid core number for node " + node.id + ": " +
                       std::to_string(node.core) + " (valid range: 0-" + std::to_string(max_core) +
                       ")");
    }

    for (const auto& lock : node.locks) {
      if (defined_locks.count(lock) == 0) {
        result.add_error("Node " + node.id + " uses undefined lock: " + lock);
      }
      if (get_lock_type(lock) == LockType::UNKNOWN) {
        result.add_error("Node " + node.id + " uses invalid lock prefix '" + lock +
                         "': must start with 'mutex' or 'spin'");
      }
    }

    if (node.type == kNodeTypeTask) {
      const int expected_count = calculate_time_interval_count(static_cast<int>(node.locks.size()));
      const int actual_count = static_cast<int>(node.time.size());
      if (actual_count != expected_count) {
        result.add_error("Node " + node.id + " has " + std::to_string(node.locks.size()) +
                         " lock(s) but " + std::to_string(actual_count) +
                         " time interval(s) (expected " + std::to_string(expected_count) + ")");
      }
    }

    for (const auto& time_range : node.time) {
      if (time_range.first > time_range.second) {
        result.add_error("Invalid time interval for node " + node.id + ": " +
                         format_range(time_range));
      }
    }

    if (node.type == kNodeTypeFork || node.type == kNodeTypeJoin) {
      // Fork/Join may carry a firing interval, a core, and a priority; locks
      // are still meaningless for a synchronisation transition.
      if (!node.locks.empty()) {
        result.add_warning("Node " + node.id + " is " + node.type +
                           " but declares locks (ignored)");
      }
      if (node.time.size() > 1) {
        result.add_warning("Node " + node.id + " is " + node.type +
                           " with multiple time intervals; only the first is "
                           "used");
      }
      if (node.has_core && node.core != -1 && (node.core < 0 || node.core > max_core)) {
        result.add_error("Invalid core number for " + node.type + " node " + node.id + ": " +
                         std::to_string(node.core) + " (valid: -1 for control core, or 0-" +
                         std::to_string(max_core) + ")");
      }
    }
  }

  for (const auto& edge : graph_.edges) {
    if (node_ids.count(edge.source) == 0) {
      result.add_error("Edge references unknown source node: " + edge.source);
    }
    if (node_ids.count(edge.target) == 0) {
      result.add_error("Edge references unknown target node: " + edge.target);
    }
  }

  for (const auto& start_task : graph_.start_tasks) {
    if (node_ids.count(start_task.task) == 0) {
      result.add_error("Start task references unknown node: " + start_task.task);
      continue;
    }
    if (!is_task_type(node_types.at(start_task.task))) {
      result.add_error("Start task must reference a task node: " + start_task.task);
      continue;
    }
    if (start_task.tokens < 0) {
      result.add_error("Start task token count must be non-negative: " + start_task.task);
    }
    if (has_incoming_edge(graph_, start_task.task)) {
      result.add_warning("Start task " + start_task.task + " has predecessor edges");
    }
  }

  for (const auto& end_task : graph_.end_tasks) {
    if (node_ids.count(end_task) == 0) {
      result.add_error("End task references unknown node: " + end_task);
      continue;
    }
    if (!is_task_type(node_types.at(end_task))) {
      result.add_error("End task must reference a task node: " + end_task);
      continue;
    }
    if (has_outgoing_edge(graph_, end_task)) {
      result.add_warning("End task " + end_task + " has successor edges");
    }
  }

  for (const auto& periodic_task : graph_.periodic_tasks) {
    if (node_ids.count(periodic_task.task) == 0) {
      result.add_error("Periodic task references unknown node: " + periodic_task.task);
      continue;
    }
    if (!is_task_type(node_types.at(periodic_task.task))) {
      result.add_error("Periodic task must reference a task node: " + periodic_task.task);
      continue;
    }
    if (periodic_task.period <= 0) {
      result.add_error("Periodic task period must be positive: " + periodic_task.task);
    }
    if (has_self_loop_edge(graph_, periodic_task.task)) {
      result.add_warning("Periodic task " + periodic_task.task +
                         " already has a self-loop release edge");
    }
  }

  const bool has_task_nodes =
      std::any_of(graph_.nodes.begin(), graph_.nodes.end(),
                  [](const JsonNode& node) { return node.type == kNodeTypeTask; });
  if (!has_task_nodes && !graph_.nodes.empty()) {
    result.add_warning("No task nodes found in graph");
  }

  return result;
}

std::string Parser::to_dot_string() const {
  std::ostringstream oss;

  oss << "digraph " << graph_.name << " {\n";
  oss << "  rankdir=LR;\n";
  oss << "  node [shape=box];\n\n";

  for (const auto& node : graph_.nodes) {
    oss << "  " << node.id << " [label=\"" << build_node_label(node) << "\"];\n";
  }

  oss << "\n";

  for (const auto& edge : graph_.edges) {
    oss << "  " << edge.source << " -> " << edge.target;
    if (!edge.label.empty() || !edge.style.empty()) {
      oss << " [";
      if (!edge.label.empty()) {
        oss << "xlabel=\"" << edge.label << "\"";
      }
      if (!edge.style.empty()) {
        if (!edge.label.empty()) {
          oss << ", ";
        }
        oss << "style=\"" << edge.style << "\"";
      }
      oss << "]";
    }
    oss << ";\n";
  }

  oss << "}\n";
  return oss.str();
}

NodeType JsonNode::to_node_type() const {
  if (type == kNodeTypeTask) {
    TaskNode task;
    task.name = id;
    task.priority = priority;
    task.core = core;
    task.time = time;
    task.lock = locks;
    task.task_type = TaskType::NORMAL;
    return task;
  }
  if (type == kNodeTypeFork) {
    ForkTask fork{id};
    if (!time.empty()) {
      fork.time = time.front();
    }
    fork.core = has_core ? core : -1;
    fork.priority = has_priority ? priority : 0;
    return fork;
  }
  if (type == kNodeTypeJoin) {
    JoinTask join{id};
    if (!time.empty()) {
      join.time = time.front();
    }
    join.core = has_core ? core : -1;
    join.priority = has_priority ? priority : 0;
    return join;
  }
  return EmptyTask{id};
}

std::string node_to_dot_label(const NodeType& node) {
  return visit_node(node, [](const auto& typed_node) -> std::string {
    using Node = std::decay_t<decltype(typed_node)>;

    if constexpr (std::is_same_v<Node, TaskNode>) {
      std::ostringstream oss;
      oss << typed_node.name << "\\ntask\\nprio=" << typed_node.priority
          << " core=" << typed_node.core << "\\n";

      for (size_t i = 0; i < typed_node.time.size(); ++i) {
        if (i > 0) {
          oss << ", ";
        }
        oss << get_time_interval_label(static_cast<int>(i), typed_node.lock) << " "
            << format_range(typed_node.time[i]);
      }

      oss << "\\nlocks=" << format_locks_with_type(typed_node.lock);
      return oss.str();
    }

    if constexpr (std::is_same_v<Node, ForkTask>) {
      return typed_node.name + "\\nfork";
    }
    if constexpr (std::is_same_v<Node, JoinTask>) {
      return typed_node.name + "\\njoin";
    }
    return typed_node.name + "\\nempty";
  });
}

}  // namespace parse
