#include "lower/tdg2pn/resource_lowering.h"

#include <map>
#include <set>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "lower/tdg2pn/tdg2pn_common.h"

namespace converter::detail {

namespace {

void add_cpu_resource(petri::PTPN& ptpn, int cpus, int cores_per_cpu) {
  for (int core = 0; core < cpus; ++core) {
    const std::string core_name = "core" + std::to_string(core);
    const petri::PlaceId core_place = ptpn.add_place(core_name, cores_per_cpu);
    ptpn.cpus_place.push_back(core_place);
    ptpn.set_initial_marking(core_place, cores_per_cpu);
  }
  spdlog::info("[TDG2PN] Created core resources");
}

void add_lock_resource(petri::PTPN& ptpn, const std::set<std::string>& locks_name) {
  if (locks_name.empty()) {
    spdlog::info("[TDG2PN] TDG without locks");
    return;
  }

  for (const auto& lock_name : locks_name) {
    const petri::PlaceId lock_place = ptpn.add_place(lock_name, 1);
    ptpn.locks_place.emplace(lock_name, lock_place);
    ptpn.set_initial_marking(lock_place, 1);
  }
  spdlog::info("[TDG2PN] Created lock resources");
}

void task_bind_cpu_resource(petri::PTPN& ptpn, const std::vector<NodeType>& all_task) {
  for (const auto& node : all_task) {
    const auto* task = as_task_node(node);
    if (!task) {
      continue;
    }

    const auto chain_it = ptpn.node_pn_map.find(task->name);
    if (chain_it == ptpn.node_pn_map.end()) {
      continue;
    }

    const auto& chain = chain_it->second;
    if (chain.size() < TaskChainLayout::kMinLength) {
      continue;
    }

    ptpn.set_pre_arc(ptpn.cpus_place[task->core], chain[TaskChainLayout::kGetCore].as_transition(),
                     1);
    ptpn.set_post_arc(chain[chain.size() - 2].as_transition(), ptpn.cpus_place[task->core], 1);
  }
}

void bind_task_locks(petri::PTPN& ptpn, const std::string& task_name,
                     const std::vector<std::string>& lock_types,
                     const std::vector<petri::NodeRef>& task_pt_chain,
                     const std::map<std::string, std::vector<std::string>>& task_locks) {
  if (task_pt_chain.size() < TaskChainLayout::kMinLength) {
    spdlog::debug("[TDG2PN] Skip chain for {}: too short for locks", task_name);
    return;
  }

  const auto task_locks_it = task_locks.find(task_name);
  if (task_locks_it == task_locks.end()) {
    spdlog::warn("[TDG2PN] No locks found for task: {}", task_name);
    return;
  }

  const size_t lock_count = task_locks_it->second.size();
  for (size_t lock_index = 0; lock_index < lock_count; ++lock_index) {
    const std::string& lock_type = lock_types[lock_index];

    const size_t acquire_chain_index = TaskChainLayout::lock_acquire_transition(lock_index);
    const size_t release_chain_index =
        TaskChainLayout::lock_release_transition(task_pt_chain.size(), lock_index);

    if (acquire_chain_index >= task_pt_chain.size() ||
        release_chain_index >= task_pt_chain.size()) {
      throw std::runtime_error("Task chain layout does not match lock structure for: " + task_name);
    }

    const petri::TransitionId acquire_transition =
        task_pt_chain[acquire_chain_index].as_transition();
    const petri::TransitionId release_transition =
        task_pt_chain[release_chain_index].as_transition();

    const auto lock_it = ptpn.locks_place.find(lock_type);
    if (lock_it == ptpn.locks_place.end()) {
      throw std::runtime_error("Lock place not found: " + lock_type);
    }

    ptpn.set_pre_arc(lock_it->second, acquire_transition, 1);
    ptpn.set_post_arc(release_transition, lock_it->second, 1);

    spdlog::debug("[TDG2PN] Bound lock {} to task {}", lock_type, task_name);
  }
}

void task_bind_lock_resource(petri::PTPN& ptpn, const std::vector<NodeType>& all_task,
                             const std::map<std::string, std::vector<std::string>>& task_locks) {
  if (task_locks.empty()) {
    spdlog::info("[TDG2PN] No task locks to bind");
    return;
  }

  for (const auto& node : all_task) {
    const auto* task = as_task_node(node);
    if (!task) {
      continue;
    }

    const auto chain_it = ptpn.node_pn_map.find(task->name);
    if (chain_it == ptpn.node_pn_map.end()) {
      continue;
    }

    bind_task_locks(ptpn, task->name, task->lock, chain_it->second, task_locks);
  }

  spdlog::info("[TDG2PN] Completed lock resource binding for all tasks");
}

}  // namespace

void add_resources_and_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  if (!is_resume_policy(tdg.policy)) {
    add_cpu_resource(ptpn, tdg.num_cpus, tdg.cores_per_cpu);
  } else {
    // One task per core, matching the physical model (cores_per_cpu parallelism
    // is intentionally not used here).
    for (int cpu = 0; cpu < tdg.num_cpus; ++cpu) {
      ptpn.core_parallelism[cpu] = 1;
    }
  }
  add_lock_resource(ptpn, tdg.lock_set);
  if (!is_resume_policy(tdg.policy)) {
    task_bind_cpu_resource(ptpn, tdg.all_task);
  }
  task_bind_lock_resource(ptpn, tdg.all_task, tdg.task_locks_map);
}

}  // namespace converter::detail
