#include "tdg/tdg.h"

#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>

#include "../json/json.h"

namespace tdg {

namespace {

// Registers a parsed node into the TDG lookup tables.
void register_node(TDG& tdg, const NodeType& node_type, bool log_node) {
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

// Populates TDG fields from a successfully parsed JSON graph.
void load_from_parser(TDG& tdg, const parse::Parser& parser, bool log_nodes) {
  tdg.num_cpus = parser.get_num_cpus();
  tdg.cores_per_cpu = parser.get_cores_per_cpu();
  tdg.task_place_capacity = parser.get_task_place_capacity();
  tdg.policy = parser.get_policy();
  tdg.start_tasks = parser.get_start_tasks();
  tdg.end_tasks = parser.get_end_tasks();
  tdg.periodic_tasks = parser.get_periodic_tasks();

  for (const auto& json_node : parser.get_nodes()) {
    register_node(tdg, json_node.to_node_type(), log_nodes);
  }

  tdg.tdg_edges.clear();
  tdg.tdg_edges.reserve(parser.get_edges().size());
  for (const auto& edge : parser.get_edges()) {
    tdg.tdg_edges.push_back({edge.source, edge.target, edge.label, edge.style});
    spdlog::debug("[TDG] Edge: {} -> {} (style={})", edge.source, edge.target, edge.style);
  }
}

}  // namespace

void TDG::parse_json(const std::string& json_file) {
  spdlog::info("[TDG] Starting JSON parsing: {}", json_file);

  parse::Parser parser;
  const auto result = parser.parse_file(json_file);
  if (!result.success) {
    spdlog::error("[TDG] JSON parsing failed: {}", result.error_message);
    return;
  }

  spdlog::info("[TDG] Configuration: {} CPUs, {} cores per CPU", parser.get_num_cpus(),
               parser.get_cores_per_cpu());

  load_from_parser(*this, parser, /*log_nodes=*/true);

  spdlog::info("[TDG] JSON parsing completed: {} nodes, {} edges", nodes_type.size(),
               tdg_edges.size());
}

void TDG::parse_json_string(const std::string& json_content) {
  parse::Parser parser;
  const auto result = parser.parse_string(json_content);
  if (!result.success) {
    throw std::runtime_error("JSON parsing failed: " + result.error_message);
  }

  load_from_parser(*this, parser, /*log_nodes=*/false);
}

std::string TDG::to_dot_string() const {
  std::ostringstream oss;

  oss << "digraph G {\n";

  for (const auto& [name, node] : nodes_type) {
    oss << "    " << name << " [label = \"" << parse::node_to_dot_label(node) << "\";];\n";
  }

  for (const auto& edge : tdg_edges) {
    oss << "    " << edge.source << " -> " << edge.target;
    if (!edge.label.empty() || !edge.style.empty()) {
      oss << " [";
      if (!edge.label.empty()) {
        oss << "xlabel = \"" << edge.label << "\"";
      }
      if (!edge.style.empty()) {
        if (!edge.label.empty()) {
          oss << "; ";
        }
        oss << "style = \"" << edge.style << "\"";
      }
      oss << ";]";
    }
    oss << ";\n";
  }

  oss << "}\n";
  return oss.str();
}

void TDG::export_to_dot(const std::string& output_path) {
  spdlog::info("[DOT] Exporting to: {}", output_path);

  std::ofstream file(output_path);
  if (!file.is_open()) {
    spdlog::error("[DOT] Failed to create file: {}", output_path);
    return;
  }

  file << to_dot_string();

  spdlog::info("[DOT] Exported {} nodes, {} edges to {}", nodes_type.size(), tdg_edges.size(),
               output_path);
}

}  // namespace tdg
