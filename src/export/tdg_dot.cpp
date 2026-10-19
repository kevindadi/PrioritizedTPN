#include "export/tdg_dot.h"

#include <fstream>
#include <sstream>
#include <string>

#include <spdlog/spdlog.h>

#include "model/tdg.h"
#include "parse/json.h"

namespace tdg {

std::string to_dot_string(const TDG& tdg) {
  std::ostringstream oss;

  oss << "digraph G {\n";

  for (const auto& [name, node] : tdg.nodes_type) {
    oss << "    " << name << " [label = \"" << parse::node_to_dot_label(node) << "\";];\n";
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
  // Flush first so a delayed write error (for example ENOSPC on /dev/full) is
  // observed before the success log; close() can also report a late error.
  file.flush();
  if (!file) {
    spdlog::error("[DOT] Failed to write file: {}", output_path);
    return false;
  }
  file.close();
  if (file.fail()) {
    spdlog::error("[DOT] Failed to close file: {}", output_path);
    return false;
  }

  spdlog::info("[DOT] Exported {} nodes, {} edges to {}", tdg.nodes_type.size(),
               tdg.tdg_edges.size(), output_path);
  return true;
}

}  // namespace tdg
