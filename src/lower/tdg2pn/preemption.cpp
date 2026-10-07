#include "lower/tdg2pn/preemption.h"

#include <algorithm>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "lower/tdg2pn/tdg2pn_common.h"

namespace converter::detail {

namespace {

std::string format_core_priority_order(int core_id, const std::vector<std::string>& tasks,
                                       const std::unordered_map<std::string, int>& tasks_priority,
                                       const std::string& prefix) {
  std::ostringstream oss;
  oss << prefix << " Core " << core_id << " priority order: ";

  bool first = true;
  for (const auto& task : tasks) {
    if (!first) {
      oss << " > ";
    }
    first = false;
    oss << task << "(" << tasks_priority.at(task) << ")";
  }

  if (first) {
    oss << "(none)";
  }

  return oss.str();
}

std::unordered_map<int, std::vector<std::string>> classify_tdg_priority(const tdg::TDG& tdg) {
  std::unordered_map<int, std::vector<std::string>> core_task;

  for (const auto& node : tdg.all_task) {
    if (const auto* task = as_task_node(node)) {
      core_task[task->core].push_back(task->name);
    }
  }

  for (auto& [core_id, tasks] : core_task) {
    std::sort(tasks.begin(), tasks.end(), [&](const std::string& left, const std::string& right) {
      return tdg.tasks_priority.at(left) > tdg.tasks_priority.at(right);
    });
    spdlog::info("{}", format_core_priority_order(core_id, tasks, tdg.tasks_priority, "[TDG2PN]"));
  }

  return core_task;
}

std::unordered_map<std::string, int> build_preempt_priorities(
    const std::vector<std::string>& tasks, const std::unordered_map<std::string, TaskConfig>& tc,
    int aggressor_priority) {
  std::unordered_map<std::string, int> priorities;

  for (const auto& task_name : tasks) {
    const auto task_it = tc.find(task_name);
    if (task_it == tc.end()) {
      continue;
    }

    if (task_it->second.priority >= aggressor_priority) {
      continue;
    }

    priorities[task_name] = aggressor_priority;
  }

  return priorities;
}

void add_restart_preemption(petri::PTPN& ptpn,
                            const std::unordered_map<int, std::vector<std::string>>& core_task,
                            const std::unordered_map<std::string, TaskConfig>& tc,
                            const std::unordered_map<std::string, NodeType>& nodes_type) {
  spdlog::info("[TDG2PN] Starting fixed-priority restart preemption addition");

  auto handle_task_preemption = [&](const std::string& l_t_name, const std::string& h_t_name,
                                    const TaskConfig& l_tc, const TaskConfig& h_tc,
                                    const std::vector<size_t>& l_t_pn,
                                    const std::vector<size_t>& h_t_pn, int preempt_priority,
                                    bool /* is_interrupt */) {
    if (l_t_pn.size() < 5 || h_t_pn.size() < 5) {
      spdlog::warn(
          "[TDG2PN] Task chain too short, skipping restart preemption: {} "
          "<- {}",
          l_t_name, h_t_name);
      return;
    }

    const size_t l_exec = l_t_pn[3];
    if (l_exec < ptpn.transitions.size()) {
      ptpn.transitions[l_exec].suspendable = true;
    }

    const size_t l_entry = l_t_pn[0];
    const size_t l_preempt_place = l_t_pn[2];
    const size_t h_entry = h_t_pn[0];
    const size_t h_ready = h_t_pn[2];

    const std::string preempt_name =
        h_t_name + "_restart_preempt_" + l_t_name + "_" + std::to_string(ptpn.node_index++);
    const petri::TimeInterval preempt_interval(0, 0);
    const size_t preempt_trans =
        ptpn.add_transition(preempt_name, preempt_interval, preempt_priority, h_tc.core, false);

    ptpn.set_pre_arc(h_entry, preempt_trans, 1);
    ptpn.set_pre_arc(l_preempt_place, preempt_trans, 1);
    ptpn.set_post_arc(preempt_trans, h_ready, 1);
    ptpn.set_post_arc(preempt_trans, l_entry, 1);

    if (!l_tc.locks.empty()) {
      constexpr size_t MIN_CHAIN_LENGTH = 9;
      if (l_t_pn.size() < MIN_CHAIN_LENGTH) {
        spdlog::debug("[TDG2PN] Task chain too short for lock restart preemption: {}", l_t_name);
        return;
      }

      for (size_t i = 0; i < l_tc.locks.size(); ++i) {
        if (l_tc.locks[i].find("spin") != std::string::npos) {
          break;
        }

        const size_t idx = l_t_pn.size() - 2 - 2 * (i + 1);
        if (idx < ptpn.transitions.size()) {
          ptpn.transitions[idx].suspendable = true;
        }

        const size_t lock_preempt_place = l_t_pn[idx - 1];
        const std::string lock_preempt_name = h_t_name + "_restart_lock_preempt_" + l_t_name + "_" +
                                              std::to_string(ptpn.node_index++);
        const petri::TimeInterval lock_preempt_interval(0, 0);
        const size_t lock_preempt_trans = ptpn.add_transition(
            lock_preempt_name, lock_preempt_interval, preempt_priority, h_tc.core, false);

        ptpn.set_pre_arc(h_entry, lock_preempt_trans, 1);
        ptpn.set_pre_arc(lock_preempt_place, lock_preempt_trans, 1);
        ptpn.set_post_arc(lock_preempt_trans, h_ready, 1);
        ptpn.set_post_arc(lock_preempt_trans, l_entry, 1);
      }
    }
  };

  for (const auto& [core_id, tasks] : core_task) {
    spdlog::debug("[TDG2PN] Processing restart preemption for core {}", core_id);

    for (size_t i = 0; i < tasks.size(); ++i) {
      const std::string& h_t_name = tasks[i];
      const auto h_t_it = tc.find(h_t_name);
      if (h_t_it == tc.end()) {
        continue;
      }
      const TaskConfig& h_tc = h_t_it->second;
      const auto preempt_priorities = build_preempt_priorities(tasks, tc, h_tc.priority);

      for (size_t j = i + 1; j < tasks.size(); ++j) {
        const std::string& l_t_name = tasks[j];

        const auto l_t_it = tc.find(l_t_name);
        if (l_t_it == tc.end()) {
          continue;
        }
        const auto preempt_priority_it = preempt_priorities.find(l_t_name);
        if (preempt_priority_it == preempt_priorities.end()) {
          continue;
        }

        const TaskConfig& l_tc = l_t_it->second;
        if (l_tc.priority == h_tc.priority) {
          continue;
        }

        const auto l_t_pns_it = ptpn.node_pn_map.find(l_t_name);
        const auto h_t_pns_it = ptpn.node_pn_map.find(h_t_name);
        if (l_t_pns_it == ptpn.node_pn_map.end() || h_t_pns_it == ptpn.node_pn_map.end()) {
          spdlog::warn("[TDG2PN] Cannot find task chain: {} or {}", l_t_name, h_t_name);
          continue;
        }

        const std::vector<size_t>& h_t_pn = h_t_pns_it->second;
        bool is_interrupt = false;
        const auto node_type_it = nodes_type.find(h_t_name);
        if (node_type_it != nodes_type.end() &&
            std::holds_alternative<TaskNode>(node_type_it->second)) {
          const auto& task = std::get<TaskNode>(node_type_it->second);
          is_interrupt = (task.task_type == TaskType::INTERRUPT);
        }

        handle_task_preemption(l_t_name, h_t_name, l_tc, h_tc, l_t_pns_it->second, h_t_pn,
                               preempt_priority_it->second, is_interrupt);
      }
    }
  }

  spdlog::info("[TDG2PN] Fixed-priority restart preemption tasks added");
}

class ResumePreemptionStrategy : public PreemptionStrategy {
 public:
  void apply(petri::PTPN& /*ptpn*/, const tdg::TDG& /*tdg*/) const override {
    spdlog::info(
        "[TDG2PN] Resume preemption is expressed by the analysis engine "
        "(per-core priority filter + execution-clock freeze); no structural "
        "preempt/resume sub-net is generated");
  }
};

class RestartPreemptionStrategy : public PreemptionStrategy {
 public:
  void apply(petri::PTPN& ptpn, const tdg::TDG& tdg) const override {
    spdlog::info("[TDG2PN] Creating fixed-priority restart preemption relations");
    std::unordered_map<std::string, TaskConfig> tasks_config;
    for (const auto& node : tdg.all_task) {
      if (const auto* task = as_task_node(node)) {
        tasks_config.emplace(task->name,
                             TaskConfig{task->core, task->priority, task->time, task->lock});
      }
    }
    const auto core_task = classify_tdg_priority(tdg);
    add_restart_preemption(ptpn, core_task, tasks_config, tdg.nodes_type);
  }
};

class NoPreemptionStrategy : public PreemptionStrategy {
 public:
  void apply(petri::PTPN& /*ptpn*/, const tdg::TDG& /*tdg*/) const override {
    spdlog::info("[TDG2PN] Skipping preemption expansion for non-fixed policy");
  }
};

}  // namespace

std::unique_ptr<PreemptionStrategy> make_preemption_strategy(SchedulePolicy policy) {
  if (policy == SchedulePolicy::FIXED || policy == SchedulePolicy::FIXED_PRIOR_WITH_RESUME) {
    return std::make_unique<ResumePreemptionStrategy>();
  }
  if (policy == SchedulePolicy::FIXED_PRIOR_WITH_RESTART) {
    return std::make_unique<RestartPreemptionStrategy>();
  }
  return std::make_unique<NoPreemptionStrategy>();
}

}  // namespace converter::detail
