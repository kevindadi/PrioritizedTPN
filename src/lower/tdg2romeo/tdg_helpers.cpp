#include "tdg_helpers.h"

#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>
#include <sstream>

namespace romeo {

namespace {

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

bool parse_time_bound(const std::string& text, int& out) {
  const std::string trimmed = trim(text);
  if (trimmed.empty()) {
    return false;
  }
  if (trimmed == "inf" || trimmed == "INF" || trimmed == "infinity") {
    out = kInfTime;
    return true;
  }
  try {
    out = std::stoi(trimmed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

RomeoTimeInterval parse_edge_interval(const std::string& label, const std::string& source_name,
                                      const std::string& target_name) {
  std::string body = trim(label);
  if (!body.empty() && (body.front() == '[' || body.front() == '(') &&
      (body.back() == ']' || body.back() == ')')) {
    body = trim(body.substr(1, body.size() - 2));
  }
  if (body.empty()) {
    return RomeoTimeInterval::immediate();
  }

  const auto comma = body.find(',');
  int earliest = 0;
  int latest = 0;
  bool ok = false;
  if (comma == std::string::npos) {
    ok = parse_time_bound(body, earliest);
    latest = earliest;
  } else {
    ok = parse_time_bound(body.substr(0, comma), earliest) &&
         parse_time_bound(body.substr(comma + 1), latest);
  }

  if (!ok || earliest == kInfTime || (latest != kInfTime && latest < earliest)) {
    spdlog::warn("[TDG2ROMEO] Invalid edge label '{}' on {} -> {}; using [0,0]", label, source_name,
                 target_name);
    return RomeoTimeInterval::immediate();
  }
  return RomeoTimeInterval::closed(earliest, latest);
}

bool has_non_self_successor(const tdg::TDG& tdg, const std::string& task_name) {
  return std::any_of(tdg.tdg_edges.begin(), tdg.tdg_edges.end(),
                     [&](const TdgEdge& edge) { return edge.leaves(task_name); });
}

bool has_self_loop_release(const tdg::TDG& tdg, const std::string& task_name) {
  return std::any_of(tdg.tdg_edges.begin(), tdg.tdg_edges.end(), [&](const TdgEdge& edge) {
    return edge.source == task_name && edge.is_self_loop();
  });
}

std::unordered_map<int, std::vector<std::string>> group_tasks_by_core(const tdg::TDG& tdg) {
  std::unordered_map<int, std::vector<std::string>> by_core;
  for (const auto& node : tdg.all_task) {
    const auto* task = as_task_node(node);
    if (task == nullptr) {
      continue;
    }
    by_core[task->core].push_back(task->name);
  }
  for (auto& [core_id, tasks] : by_core) {
    std::sort(tasks.begin(), tasks.end(), [&](const std::string& left, const std::string& right) {
      return tdg.tasks_priority.at(left) > tdg.tasks_priority.at(right);
    });
    (void)core_id;
  }
  return by_core;
}

std::string core_place_name(int core_id) {
  return "core" + std::to_string(core_id);
}

std::string core_busy_place_name(int core_id) {
  return "core" + std::to_string(core_id) + "_busy";
}

std::string place_name(const std::string& task, const std::string& suffix) {
  return task + suffix;
}

std::string assignment_expr(const std::string& place, int delta) {
  std::ostringstream out;
  out << place << " = " << place;
  if (delta > 0) {
    out << " + " << delta;
  } else if (delta < 0) {
    out << " - " << -delta;
  }
  return out.str();
}

std::string guard_ge(const std::string& place, int weight) {
  return place + " >= " + std::to_string(weight);
}

std::string guard_and(const std::vector<std::string>& clauses) {
  if (clauses.empty()) {
    return "true";
  }
  std::ostringstream out;
  for (size_t i = 0; i < clauses.size(); ++i) {
    if (i > 0) {
      out << " and ";
    }
    out << clauses[i];
  }
  return out.str();
}

}  // namespace romeo
