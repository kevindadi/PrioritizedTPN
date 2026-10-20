#include "export/tdg_dot.h"

#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "common/checked_output.h"
#include "model/tdg.h"

namespace {

std::string format_range(const std::pair<int, int>& range) {
  return "[" + std::to_string(range.first) + ", " + std::to_string(range.second) + "]";
}

// Each lock adds a pre-CS, CS, and post-CS segment: total = 2 * locks + 1.
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

std::string get_lock_type_short(const std::string& lock_name) {
  if (lock_name.rfind("mutex", 0) == 0) {
    return "[M]";
  }
  if (lock_name.rfind("spin", 0) == 0) {
    return "[S]";
  }
  return "[?]";
}

std::string format_locks_with_type(const std::vector<std::string>& locks) {
  if (locks.empty()) {
    return "none";
  }

  std::ostringstream oss;
  bool first = true;
  for (const std::string& lock : locks) {
    if (!first) {
      oss << " ";
    }
    first = false;
    oss << get_lock_type_short(lock) << lock;
  }
  return oss.str();
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

}  // namespace

namespace tdg {

std::string to_dot_string(const TDG& tdg) {
  std::ostringstream oss;

  oss << "digraph G {\n";

  for (const auto& [name, node] : tdg.nodes_type) {
    oss << "    " << name << " [label = \"" << node_to_dot_label(node) << "\";];\n";
  }

  for (const auto& edge : tdg.tdg_edges) {
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

bool export_to_dot(const TDG& tdg, const std::string& output_path) {
  spdlog::info("[DOT] Exporting to: {}", output_path);

  std::ofstream file(output_path);
  if (!file.is_open()) {
    spdlog::error("[DOT] Failed to create file: {}", output_path);
    return false;
  }

  file << to_dot_string(tdg);
  // Flush and close before logging success so delayed write errors (for
  // example ENOSPC on /dev/full) are observed.
  if (!ptpn::finish_output(file)) {
    spdlog::error("[DOT] Failed to write file: {}", output_path);
    return false;
  }

  spdlog::info("[DOT] Exported {} nodes, {} edges to {}", tdg.nodes_type.size(),
               tdg.tdg_edges.size(), output_path);
  return true;
}

}  // namespace tdg
