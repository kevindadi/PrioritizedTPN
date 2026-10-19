#include "parse/json_writer.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "model/types.h"

namespace parse {

namespace {

template <typename Node>
void collect_sorted(const tdg::TDG& tdg, std::vector<const Node*>& nodes) {
  for (const auto& [name, node] : tdg.nodes_type) {
    (void)name;
    if (const auto* typed = std::get_if<Node>(&node)) {
      nodes.push_back(typed);
    }
  }
  std::sort(nodes.begin(), nodes.end(),
            [](const Node* lhs, const Node* rhs) { return lhs->name < rhs->name; });
}

nlohmann::json task_to_json(const tdg::TDG& tdg, const TaskNode& task) {
  nlohmann::json node = nlohmann::json::object();
  node["id"] = task.name;
  node["type"] = "task";
  node["priority"] = task.priority;
  node["core"] = task.core;
  node["time"] = nlohmann::json::array();
  for (const auto& interval : task.time) {
    node["time"].push_back(nlohmann::json::array({interval.first, interval.second}));
  }
  if (!task.lock.empty()) {
    node["locks"] = task.lock;
  }
  node["deadline"] = task.deadline;

  const auto allowed = tdg.task_allowed_cores.find(task.name);
  node["allowed_cores"] =
      allowed != tdg.task_allowed_cores.end() ? allowed->second : std::vector<int>{task.core};

  const auto provenance = tdg.task_wcet_provenance.find(task.name);
  if (provenance != tdg.task_wcet_provenance.end()) {
    node["wcet_kind"] = provenance->second.kind;
    if (!provenance->second.method.empty()) {
      node["wcet_method"] = provenance->second.method;
    }
    if (!provenance->second.artifact.empty()) {
      node["wcet_artifact"] = provenance->second.artifact;
    }
  } else {
    node["wcet_kind"] = "model";
  }
  return node;
}

}  // namespace

nlohmann::json tdg_to_json(const tdg::TDG& tdg) {
  nlohmann::json document = nlohmann::json::object();
  document["graph"] = nlohmann::json{{"name", "G"}};

  nlohmann::json configuration = nlohmann::json::object();
  configuration["num_cpus"] = tdg.num_cpus;
  configuration["cores_per_cpu"] = tdg.cores_per_cpu;
  configuration["task_place_capacity"] = tdg.task_place_capacity;
  if (!tdg.lock_set.empty()) {
    configuration["shared_locks"] =
        std::vector<std::string>(tdg.lock_set.begin(), tdg.lock_set.end());
  }
  configuration["policy"] = schedule_policy_to_string(tdg.policy);

  if (!tdg.start_tasks.empty()) {
    configuration["start"] = nlohmann::json::array();
    std::vector<StartBinding> starts = tdg.start_tasks;
    std::sort(starts.begin(), starts.end(),
              [](const StartBinding& lhs, const StartBinding& rhs) { return lhs.task < rhs.task; });
    for (const auto& binding : starts) {
      configuration["start"].push_back(
          nlohmann::json{{"task", binding.task}, {"tokens", binding.tokens}});
    }
  }
  if (!tdg.end_tasks.empty()) {
    std::vector<std::string> ends = tdg.end_tasks;
    std::sort(ends.begin(), ends.end());
    configuration["end"] = ends;
  }
  if (!tdg.periodic_tasks.empty()) {
    configuration["periodic"] = nlohmann::json::array();
    std::vector<PeriodicBinding> periodic = tdg.periodic_tasks;
    std::sort(
        periodic.begin(), periodic.end(),
        [](const PeriodicBinding& lhs, const PeriodicBinding& rhs) { return lhs.task < rhs.task; });
    for (const auto& binding : periodic) {
      configuration["periodic"].push_back(
          nlohmann::json{{"task", binding.task}, {"period", binding.period}});
    }
  }
  document["configuration"] = std::move(configuration);

  std::vector<const TaskNode*> tasks;
  std::vector<const ForkTask*> forks;
  std::vector<const JoinTask*> joins;
  collect_sorted(tdg, tasks);
  collect_sorted(tdg, forks);
  collect_sorted(tdg, joins);

  document["nodes"] = nlohmann::json::array();
  for (const TaskNode* task : tasks) {
    document["nodes"].push_back(task_to_json(tdg, *task));
  }
  for (const ForkTask* fork : forks) {
    document["nodes"].push_back(nlohmann::json{{"id", fork->name}, {"type", "fork"}});
  }
  for (const JoinTask* join : joins) {
    document["nodes"].push_back(nlohmann::json{{"id", join->name}, {"type", "join"}});
  }

  std::vector<const TdgEdge*> edges;
  edges.reserve(tdg.tdg_edges.size());
  for (const auto& edge : tdg.tdg_edges) {
    edges.push_back(&edge);
  }
  std::sort(edges.begin(), edges.end(), [](const TdgEdge* lhs, const TdgEdge* rhs) {
    if (lhs->source != rhs->source) {
      return lhs->source < rhs->source;
    }
    return lhs->target < rhs->target;
  });
  document["edges"] = nlohmann::json::array();
  for (const TdgEdge* edge : edges) {
    nlohmann::json entry = nlohmann::json::object();
    entry["source"] = edge->source;
    entry["target"] = edge->target;
    if (!edge->label.empty()) {
      entry["label"] = edge->label;
    }
    if (!edge->style.empty()) {
      entry["style"] = edge->style;
    }
    document["edges"].push_back(std::move(entry));
  }

  return document;
}

bool write_tdg_json(const tdg::TDG& tdg, const std::string& output_path) {
  std::ofstream output(output_path);
  if (!output.is_open()) {
    return false;
  }
  output << tdg_to_json(tdg).dump(2) << '\n';
  return static_cast<bool>(output);
}

}  // namespace parse
