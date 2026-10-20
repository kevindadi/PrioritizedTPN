#include "export/wcet_json.h"

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "common/checked_output.h"

namespace exporting {

bool write_wcet_json(const tdg::TDG& graph, const std::string& output_path) {
  nlohmann::json wcet = nlohmann::json::object();
  wcet["tasks"] = nlohmann::json::array();

  bool all_certified = true;
  size_t task_id = 0;
  for (const auto& node : graph.all_task) {
    const TaskNode* task = as_task_node(node);
    if (task == nullptr) {
      continue;
    }

    // Use 64-bit accumulators: a legal interval sum can exceed INT_MAX even
    // though each segment fits in an int.
    long long wcet_lo = 0;
    long long wcet_hi = 0;
    for (const auto& interval : task->time) {
      wcet_lo += interval.first;
      wcet_hi += interval.second;
    }

    tdg::WcetProvenance provenance;
    if (const auto it = graph.task_wcet_provenance.find(task->name);
        it != graph.task_wcet_provenance.end()) {
      provenance = it->second;
    }
    all_certified = all_certified && provenance.kind == "certified";

    wcet["tasks"].push_back({
        {"id", task_id++},
        {"name", task->name},
        {"wcet", wcet_hi},
        {"interval", nlohmann::json::array({wcet_lo, wcet_hi})},
        {"segments", task->time.size()},
        {"wcet_kind", provenance.kind},
        {"wcet_method", provenance.method},
        {"wcet_artifact", provenance.artifact},
    });
  }
  wcet["all_certified"] = all_certified;

  std::ofstream out(output_path);
  if (!out.is_open()) {
    return false;
  }

  out << wcet.dump(2) << '\n';
  return ptpn::finish_output(out);
}

}  // namespace exporting
