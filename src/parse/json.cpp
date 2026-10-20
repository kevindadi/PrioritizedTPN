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

std::string format_time_ranges(const std::vector<std::pair<int, int>>& time_ranges) {
  return join(time_ranges, ", ", format_time_range);
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

bool is_valid_wcet_kind(const std::string& kind) {
  return kind == "model" || kind == "certified" || kind == "measured" || kind == "estimated";
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

// Converts nlohmann's 1-based byte index (see parse_error::byte) into a
// 1-based line/column pair over the in-memory source text.
ptpn::SourceLocation location_from_byte(const std::string& text, std::size_t byte,
                                        const std::string& file) {
  ptpn::SourceLocation location;
  location.file = file;
  std::size_t offset = byte > 0 ? byte - 1 : 0;
  if (offset > text.size()) {
    offset = text.size();
  }
  location.line = 1;
  location.column = 1;
  for (std::size_t i = 0; i < offset; ++i) {
    if (text[i] == '\n') {
      ++location.line;
      location.column = 1;
    } else {
      ++location.column;
    }
  }
  location.span = 1;
  return location;
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

// Each lock adds a pre-CS, CS, and post-CS segment: total = 2 * locks + 1.
int calculate_time_interval_count(int lock_count) {
  return 2 * lock_count + 1;
}

ParseResult Parser::parse_file(const std::string& file_path) {
  spdlog::info("[JSON] Starting JSON parsing: {}", file_path);

  std::ifstream file(file_path);
  if (!file.is_open()) {
    spdlog::error("[JSON] Failed to open file: {}", file_path);
    diagnostics_.clear();
    ptpn::SourceLocation location;
    location.file = file_path;
    diagnostics_.add("E-SYN-002", "Failed to open file: " + file_path, std::nullopt, location,
                     "check that the path exists and is readable");
    return {false, "Failed to open file: " + file_path, 0};
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  original_json_ = buffer.str();
  source_name_ = file_path;
  diagnostics_.clear();

  return parse_content(original_json_);
}

ParseResult Parser::parse_string(const std::string& json_content) {
  source_name_.clear();
  diagnostics_.clear();
  return parse_content(json_content);
}

ParseResult Parser::parse_content(const std::string& json_content) {
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
    diagnostics_.add("E-SYN-001", e.what(), std::nullopt,
                     location_from_byte(json_content, e.byte, source_name_),
                     "fix the JSON syntax near the reported line and column");
    return {false, e.what(), static_cast<int>(e.byte)};
  } catch (const std::exception& e) {
    spdlog::error("[JSON] Error: {}", e.what());
    std::optional<ptpn::SourceLocation> location;
    if (!source_name_.empty()) {
      ptpn::SourceLocation file_location;
      file_location.file = source_name_;
      location = std::move(file_location);
    }
    diagnostics_.add("E-VAL-033", e.what(), std::nullopt, std::move(location),
                     "check that every JSON field has the expected type and shape");
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
  node.has_deadline = node_obj.contains("deadline");
  node.has_allowed_cores = node_obj.contains("allowed_cores");
  node.priority = node_obj.value("priority", node.priority);
  node.core = node_obj.value("core", node.core);
  node.deadline = node_obj.value("deadline", node.deadline);

  if (node.has_allowed_cores) {
    node.allowed_cores = node_obj["allowed_cores"].get<std::vector<int>>();
    // Without an explicit core, the initial allocation is the first core in
    // the domain.
    if (!node.has_core && !node.allowed_cores.empty()) {
      node.core = node.allowed_cores.front();
    }
  }

  node.wcet_kind = node_obj.value("wcet_kind", node.wcet_kind);
  node.wcet_method = node_obj.value("wcet_method", node.wcet_method);
  node.wcet_artifact = node_obj.value("wcet_artifact", node.wcet_artifact);

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
  diagnostics_.remove_category("validation");

  const auto add_error = [&](const std::string& code, const std::string& message,
                             std::optional<std::string> subject = std::nullopt,
                             std::optional<std::string> fix = std::nullopt) {
    result.add_error(message);
    diagnostics_.add(code, message, std::move(subject), std::nullopt, std::move(fix));
  };
  const auto add_warning = [&](const std::string& code, const std::string& message,
                               std::optional<std::string> subject = std::nullopt,
                               std::optional<std::string> fix = std::nullopt) {
    result.add_warning(message);
    diagnostics_.add_warning(code, message, std::move(subject), std::nullopt, std::move(fix));
  };

  if (graph_.task_place_capacity < 1) {
    add_error("E-VAL-001",
              "task_place_capacity must be >= 1, got " + std::to_string(graph_.task_place_capacity),
              "config:task_place_capacity",
              "set configuration.task_place_capacity to 1 (one in-flight job per task)");
  }

  const std::set<std::string> valid_types = {kNodeTypeTask, kNodeTypeFork, kNodeTypeJoin,
                                             kNodeTypeEmpty};
  const std::set<std::string> defined_locks(graph_.shared_locks.begin(), graph_.shared_locks.end());
  if (graph_.num_cpus < 1) {
    add_error("E-VAL-002", "num_cpus must be >= 1, got " + std::to_string(graph_.num_cpus),
              "config:num_cpus", "set configuration.num_cpus to at least 1");
  }
  if (graph_.cores_per_cpu < 1) {
    add_error("E-VAL-003",
              "cores_per_cpu must be >= 1, got " + std::to_string(graph_.cores_per_cpu),
              "config:cores_per_cpu",
              "set configuration.cores_per_cpu to a positive value (1 is the supported value)");
  }
  // Lowering creates exactly num_cpus real-core resource places. The
  // `cores_per_cpu` value is a per-core capacity parameter, not an additional
  // range of core ids (see docs/scheduling-semantics.md).
  const int max_core = std::max(0, graph_.num_cpus - 1);

  std::set<std::string> node_ids;
  std::unordered_map<std::string, std::string> node_types;

  for (const auto& node : graph_.nodes) {
    if (!node_ids.insert(node.id).second) {
      add_error("E-VAL-004", "Duplicate node ID: " + node.id, "node:" + node.id,
                "rename one of the duplicate nodes or remove the duplicate");
    }
    node_types[node.id] = node.type;

    if (valid_types.count(node.type) == 0) {
      add_error("E-VAL-005", "Unknown node type: " + node.type + " for node " + node.id,
                "node:" + node.id, "use one of: task, fork, join, empty");
    }

    if (node.type == kNodeTypeTask && (node.core < 0 || node.core > max_core)) {
      add_error("E-VAL-006",
                "Invalid core number for node " + node.id + ": " + std::to_string(node.core) +
                    " (valid range: 0-" + std::to_string(max_core) + ")",
                "task:" + node.id,
                "set core to an integer in [0, num_cpus-1] (or remove it to use core 0)");
    }

    if (node.type == kNodeTypeTask && node.has_deadline && node.deadline < 0) {
      add_error("E-VAL-007",
                "Invalid deadline for node " + node.id + ": " + std::to_string(node.deadline) +
                    " (must be >= 0)",
                "task:" + node.id, "set deadline to a non-negative value or remove the field");
    }

    if (node.type == kNodeTypeTask && node.has_allowed_cores) {
      if (node.allowed_cores.empty()) {
        add_error("E-VAL-008", "Node " + node.id + " has an empty allowed_cores array",
                  "task:" + node.id, "list at least one core in allowed_cores or remove the field");
      }

      std::set<int> seen_cores;
      for (const int allowed_core : node.allowed_cores) {
        if (allowed_core < 0 || allowed_core > max_core) {
          add_error("E-VAL-009",
                    "Invalid allowed core for node " + node.id + ": " +
                        std::to_string(allowed_core) + " (valid range: 0-" +
                        std::to_string(max_core) + ")",
                    "task:" + node.id, "list only cores in [0, num_cpus-1] in allowed_cores");
        }
        if (!seen_cores.insert(allowed_core).second) {
          add_error(
              "E-VAL-010",
              "Duplicate allowed core for node " + node.id + ": " + std::to_string(allowed_core),
              "task:" + node.id, "remove duplicate entries from allowed_cores");
        }
      }

      if (!node.allowed_cores.empty() && node.has_core && seen_cores.count(node.core) == 0) {
        add_error("E-VAL-011",
                  "Core " + std::to_string(node.core) + " of node " + node.id +
                      " is not in its allowed_cores domain",
                  "task:" + node.id,
                  "include the task's core in allowed_cores or set core to an allowed one");
      }
    }

    if (node.type == kNodeTypeTask && !is_valid_wcet_kind(node.wcet_kind)) {
      add_error("E-VAL-012",
                "Invalid wcet_kind for node " + node.id + ": '" + node.wcet_kind +
                    "' (expected model, certified, measured, or estimated)",
                "task:" + node.id, "use model, certified, measured, or estimated");
    }

    for (const auto& lock : node.locks) {
      if (defined_locks.count(lock) == 0) {
        add_error("E-VAL-013", "Node " + node.id + " uses undefined lock: " + lock, "lock:" + lock,
                  "add the lock to configuration.shared_locks or remove it from the task");
      }
      if (get_lock_type(lock) == LockType::UNKNOWN) {
        add_error("E-VAL-014",
                  "Node " + node.id + " uses invalid lock prefix '" + lock +
                      "': must start with 'mutex' or 'spin'",
                  "lock:" + lock, "rename the lock to start with 'mutex' or 'spin'");
      }
    }

    if (node.type == kNodeTypeTask) {
      const int expected_count = calculate_time_interval_count(static_cast<int>(node.locks.size()));
      const int actual_count = static_cast<int>(node.time.size());
      if (actual_count != expected_count) {
        add_error("E-VAL-015",
                  "Node " + node.id + " has " + std::to_string(node.locks.size()) +
                      " lock(s) but " + std::to_string(actual_count) +
                      " time interval(s) (expected " + std::to_string(expected_count) + ")",
                  "task:" + node.id,
                  "provide 2*lock_count+1 time intervals (pre/CS/post per lock)");
      }
    }

    for (const auto& time_range : node.time) {
      if (time_range.first > time_range.second) {
        add_error("E-VAL-016",
                  "Invalid time interval for node " + node.id + ": " +
                      format_time_range(time_range),
                  "task:" + node.id, "ensure each interval is [min, max] with min <= max");
      }
    }

    if (node.type == kNodeTypeFork || node.type == kNodeTypeJoin) {
      // Fork/Join may carry a firing interval, a core, and a priority; locks
      // are still meaningless for a synchronisation transition.
      if (!node.locks.empty()) {
        add_warning("E-VAL-028",
                    "Node " + node.id + " is " + node.type + " but declares locks (ignored)",
                    "node:" + node.id, "remove locks from fork/join nodes");
      }
      if (node.time.size() > 1) {
        add_warning("E-VAL-029",
                    "Node " + node.id + " is " + node.type +
                        " with multiple time intervals; only the first is used",
                    "node:" + node.id, "keep at most one time interval for a fork/join node");
      }
      if (node.has_core && node.core != -1 && (node.core < 0 || node.core > max_core)) {
        add_error("E-VAL-017",
                  "Invalid core number for " + node.type + " node " + node.id + ": " +
                      std::to_string(node.core) + " (valid: -1 for control core, or 0-" +
                      std::to_string(max_core) + ")",
                  "node:" + node.id, "use -1 (control core) or a core in [0, num_cpus-1]");
      }
    }
  }

  for (const auto& edge : graph_.edges) {
    const std::string edge_subject = "edge:" + edge.source + "->" + edge.target;
    if (node_ids.count(edge.source) == 0) {
      add_error("E-VAL-018", "Edge references unknown source node: " + edge.source, edge_subject,
                "add a node with this id or fix the edge source");
    }
    if (node_ids.count(edge.target) == 0) {
      add_error("E-VAL-019", "Edge references unknown target node: " + edge.target, edge_subject,
                "add a node with this id or fix the edge target");
    }
  }

  for (const auto& start_task : graph_.start_tasks) {
    if (node_ids.count(start_task.task) == 0) {
      add_error("E-VAL-020", "Start task references unknown node: " + start_task.task,
                "task:" + start_task.task, "reference an existing node id");
      continue;
    }
    if (!is_task_type(node_types.at(start_task.task))) {
      add_error("E-VAL-021", "Start task must reference a task node: " + start_task.task,
                "task:" + start_task.task, "reference a node with type 'task'");
      continue;
    }
    if (start_task.tokens < 0) {
      add_error("E-VAL-022", "Start task token count must be non-negative: " + start_task.task,
                "task:" + start_task.task, "set tokens to a non-negative integer");
    }
    if (has_incoming_edge(graph_, start_task.task)) {
      add_warning("E-VAL-030", "Start task " + start_task.task + " has predecessor edges",
                  "task:" + start_task.task,
                  "remove predecessor edges or remove the start binding");
    }
  }

  for (const auto& end_task : graph_.end_tasks) {
    if (node_ids.count(end_task) == 0) {
      add_error("E-VAL-023", "End task references unknown node: " + end_task, "task:" + end_task,
                "reference an existing node id");
      continue;
    }
    if (!is_task_type(node_types.at(end_task))) {
      add_error("E-VAL-024", "End task must reference a task node: " + end_task, "task:" + end_task,
                "reference a node with type 'task'");
      continue;
    }
    if (has_outgoing_edge(graph_, end_task)) {
      add_warning("E-VAL-031", "End task " + end_task + " has successor edges", "task:" + end_task,
                  "remove successor edges or remove the end binding");
    }
  }

  for (const auto& periodic_task : graph_.periodic_tasks) {
    if (node_ids.count(periodic_task.task) == 0) {
      add_error("E-VAL-025", "Periodic task references unknown node: " + periodic_task.task,
                "task:" + periodic_task.task, "reference an existing node id");
      continue;
    }
    if (!is_task_type(node_types.at(periodic_task.task))) {
      add_error("E-VAL-026", "Periodic task must reference a task node: " + periodic_task.task,
                "task:" + periodic_task.task, "reference a node with type 'task'");
      continue;
    }
    if (periodic_task.period <= 0) {
      add_error("E-VAL-027", "Periodic task period must be positive: " + periodic_task.task,
                "task:" + periodic_task.task, "set a positive period for the periodic binding");
    }
  }

  const bool has_task_nodes =
      std::any_of(graph_.nodes.begin(), graph_.nodes.end(),
                  [](const JsonNode& node) { return node.type == kNodeTypeTask; });
  if (!has_task_nodes && !graph_.nodes.empty()) {
    add_warning("E-VAL-032", "No task nodes found in graph", "system:nodes",
                "add at least one node with type 'task'");
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
    task.deadline = deadline;
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


namespace {

// Registers a parsed node into the TDG lookup tables.
void register_node(tdg::TDG& tdg, const NodeType& node_type, bool log_node) {
  visit_node(node_type, [&](const auto& node) {
    using Node = std::decay_t<decltype(node)>;

    if constexpr (std::is_same_v<Node, TaskNode>) {
      tdg.all_task.emplace_back(node_type);
      tdg.tasks_priority.emplace(node.name, node.priority);
      tdg.nodes_type.emplace(node.name, node_type);

      for (const auto& lock : node.lock) {
        tdg.lock_set.insert(lock);
        tdg.task_locks_map[node.name].push_back(lock);
      }

      if (log_node) {
        spdlog::info("[TDG] Node '{}' -> task (priority={}, core={})", node.name, node.priority,
                     node.core);
      }
      return;
    }

    if constexpr (std::is_same_v<Node, ForkTask>) {
      tdg.nodes_type.emplace(node.name, node_type);
      if (log_node) {
        spdlog::info("[TDG] Node '{}' -> fork", node.name);
      }
      return;
    }

    if constexpr (std::is_same_v<Node, JoinTask>) {
      tdg.nodes_type.emplace(node.name, node_type);
      if (log_node) {
        spdlog::info("[TDG] Node '{}' -> join", node.name);
      }
      return;
    }

    if constexpr (std::is_same_v<Node, EmptyTask>) {
      tdg.nodes_type.emplace(node.name, node_type);
      if (log_node) {
        spdlog::info("[TDG] Node '{}' -> empty", node.name);
      }
    }
  });
}

}  // namespace

void load_tdg(const Parser& parser, tdg::TDG& tdg, bool log_nodes) {
  tdg.num_cpus = parser.get_num_cpus();
  tdg.cores_per_cpu = parser.get_cores_per_cpu();
  tdg.task_place_capacity = parser.get_task_place_capacity();
  tdg.policy = parser.get_policy();
  tdg.start_tasks = parser.get_start_tasks();
  tdg.end_tasks = parser.get_end_tasks();
  tdg.periodic_tasks = parser.get_periodic_tasks();

  for (const auto& json_node : parser.get_nodes()) {
    const NodeType node_type = json_node.to_node_type();
    if (const TaskNode* task = std::get_if<TaskNode>(&node_type)) {
      if (json_node.has_allowed_cores) {
        tdg.task_allowed_cores[task->name] = json_node.allowed_cores;
      } else {
        tdg.task_allowed_cores[task->name] = {task->core};
      }
      tdg.task_wcet_provenance[task->name] = {json_node.wcet_kind, json_node.wcet_method,
                                              json_node.wcet_artifact};
    }
    register_node(tdg, node_type, log_nodes);
  }

  tdg.tdg_edges.clear();
  tdg.tdg_edges.reserve(parser.get_edges().size());
  for (const auto& edge : parser.get_edges()) {
    tdg.tdg_edges.push_back({edge.source, edge.target, edge.label, edge.style});
    spdlog::debug("[TDG] Edge: {} -> {} (style={})", edge.source, edge.target, edge.style);
  }
}

void load_tdg_from_file(const std::string& json_file, tdg::TDG& tdg) {
  spdlog::info("[TDG] Starting JSON parsing: {}", json_file);

  Parser parser;
  const auto result = parser.parse_file(json_file);
  if (!result.success) {
    spdlog::error("[TDG] JSON parsing failed: {}", result.error_message);
    return;
  }

  spdlog::info("[TDG] Configuration: {} CPUs, {} cores per CPU", parser.get_num_cpus(),
               parser.get_cores_per_cpu());

  load_tdg(parser, tdg, /*log_nodes=*/true);

  spdlog::info("[TDG] JSON parsing completed: {} nodes, {} edges", tdg.nodes_type.size(),
               tdg.tdg_edges.size());
}

void load_tdg_from_string(const std::string& json_content, tdg::TDG& tdg) {
  Parser parser;
  const auto result = parser.parse_string(json_content);
  if (!result.success) {
    throw std::runtime_error("JSON parsing failed: " + result.error_message);
  }

  load_tdg(parser, tdg, /*log_nodes=*/false);
}

}  // namespace parse
