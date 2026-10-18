#include "lower/tdg2pn/policy_priorities.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

namespace converter::detail {

namespace {

bool computes_priorities(SchedulePolicy policy) {
  return policy == SchedulePolicy::RM || policy == SchedulePolicy::DM;
}

// Positive periodic periods, keyed by task name. A zero/negative period means
// "no period" for priority assignment.
std::unordered_map<std::string, int> collect_periods(const tdg::TDG& tdg) {
  std::unordered_map<std::string, int> periods;
  for (const auto& binding : tdg.periodic_tasks) {
    if (binding.period > 0) {
      periods[binding.task] = binding.period;
    }
  }
  return periods;
}

// Explicit task deadlines (> 0), keyed by task name.
std::unordered_map<std::string, int> collect_deadlines(const tdg::TDG& tdg) {
  std::unordered_map<std::string, int> deadlines;
  const auto record = [&](const NodeType& node) {
    if (const auto* task = as_task_node(node); task != nullptr && task->deadline > 0) {
      deadlines[task->name] = task->deadline;
    }
  };
  for (const auto& node : tdg.all_task) {
    record(node);
  }
  // Defensive: a manually assembled TDG may hold a task only in nodes_type.
  for (const auto& [name, node] : tdg.nodes_type) {
    (void)name;
    record(node);
  }
  return deadlines;
}

// Collects every task name once, keyed by name for deterministic iteration.
std::map<std::string, const TaskNode*> collect_tasks(const tdg::TDG& tdg) {
  std::map<std::string, const TaskNode*> tasks;
  for (const auto& node : tdg.all_task) {
    if (const auto* task = as_task_node(node)) {
      tasks.emplace(task->name, task);
    }
  }
  for (const auto& [name, node] : tdg.nodes_type) {
    if (const auto* task = as_task_node(node)) {
      tasks.emplace(name, task);
    }
  }
  return tasks;
}

int priority_key(SchedulePolicy policy, const TaskNode& task,
                 const std::unordered_map<std::string, int>& periods,
                 const std::unordered_map<std::string, int>& deadlines) {
  const auto period_it = periods.find(task.name);
  const int period = period_it == periods.end() ? 0 : period_it->second;

  if (policy == SchedulePolicy::RM) {
    return period;
  }

  // DM: an explicit deadline outranks the implicit deadline (= period).
  const auto deadline_it = deadlines.find(task.name);
  if (deadline_it != deadlines.end()) {
    return deadline_it->second;
  }
  return period;
}

}  // namespace

std::unordered_map<std::string, int> compute_policy_priorities(const tdg::TDG& tdg) {
  const auto tasks = collect_tasks(tdg);

  if (!computes_priorities(tdg.policy)) {
    std::unordered_map<std::string, int> current;
    current.reserve(tasks.size());
    for (const auto& [name, task] : tasks) {
      current[name] = task->priority;
    }
    return current;
  }

  const auto periods = collect_periods(tdg);
  const auto deadlines = collect_deadlines(tdg);

  // Derive each task's key; 0 means "no key" and always maps to priority 0.
  std::map<std::string, int> key_of;
  for (const auto& [name, task] : tasks) {
    key_of[name] = priority_key(tdg.policy, *task, periods, deadlines);
  }

  // Distinct keys ascending: the smallest key is the highest scheduling
  // priority and receives the largest integer rank.
  std::vector<int> distinct_keys;
  for (const auto& [name, key] : key_of) {
    (void)name;
    if (key > 0) {
      distinct_keys.push_back(key);
    }
  }
  std::sort(distinct_keys.begin(), distinct_keys.end());
  distinct_keys.erase(std::unique(distinct_keys.begin(), distinct_keys.end()), distinct_keys.end());

  std::map<int, int> rank_of_key;
  for (size_t i = 0; i < distinct_keys.size(); ++i) {
    rank_of_key[distinct_keys[i]] = static_cast<int>(distinct_keys.size() - i);
  }

  std::unordered_map<std::string, int> priorities;
  priorities.reserve(key_of.size());
  for (const auto& [name, key] : key_of) {
    priorities[name] = key > 0 ? rank_of_key.at(key) : 0;
  }
  return priorities;
}

void apply_policy_priorities(tdg::TDG& tdg) {
  if (!computes_priorities(tdg.policy)) {
    return;
  }

  const std::unordered_map<std::string, int> priorities = compute_policy_priorities(tdg);

  for (auto& node : tdg.all_task) {
    if (auto* task = as_task_node(node)) {
      const auto it = priorities.find(task->name);
      if (it != priorities.end()) {
        task->priority = it->second;
      }
    }
  }
  for (auto& [name, node] : tdg.nodes_type) {
    (void)name;
    if (auto* task = as_task_node(node)) {
      const auto it = priorities.find(task->name);
      if (it != priorities.end()) {
        task->priority = it->second;
      }
    }
  }
  for (const auto& [name, priority] : priorities) {
    tdg.tasks_priority[name] = priority;
  }
}

}  // namespace converter::detail
