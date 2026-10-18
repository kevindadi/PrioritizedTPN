#include "analysis/reachability/state_class.h"

#include <algorithm>
#include <functional>
#include <sstream>

#include "analysis/dbm/clock_state.h"

namespace state_class {

namespace {
void hash_combine(size_t& seed, size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}
}  // namespace

std::vector<TaskChain> build_task_chains(const petri::PTPN& net) {
  std::vector<std::string> names;
  names.reserve(net.node_pn_map().size());
  for (const auto& entry : net.node_pn_map()) {
    // A task must have both a chain and scheduling metadata to participate.
    if (net.task_info().find(entry.first) == net.task_info().end()) {
      continue;
    }
    if (entry.second.empty()) {
      continue;
    }
    names.push_back(entry.first);
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());

  std::vector<TaskChain> tasks;
  tasks.reserve(names.size());
  for (const std::string& name : names) {
    const auto chain_it = net.node_pn_map().find(name);
    const auto info_it = net.task_info().find(name);
    if (chain_it == net.node_pn_map().end() || info_it == net.task_info().end()) {
      continue;
    }
    TaskChain task;
    task.name = name;
    task.chain = chain_it->second;
    task.period = info_it->second.period;
    task.deadline = info_it->second.deadline;

    for (const petri::NodeRef& node : task.chain) {
      if (node.is_place()) {
        task.places.push_back(node.as_place().index());
      } else {
        task.transitions.push_back(node.as_transition().index());
      }
    }
    if (!task.places.empty()) {
      task.entry_place = task.places.front();
      task.exit_place = task.places.back();
    }
    const auto periodic_it = net.periodic_transitions().find(name);
    if (periodic_it != net.periodic_transitions().end()) {
      task.periodic_transition = periodic_it->second.index();
    }
    tasks.push_back(std::move(task));
  }
  return tasks;
}

size_t task_chain_token_count(const TaskChain& task, const std::vector<int>& marking) {
  size_t count = 0;
  for (size_t place : task.places) {
    if (place < marking.size() && marking[place] > 0) {
      count += static_cast<size_t>(marking[place]);
    }
  }
  return count;
}

bool task_in_flight(const TaskChain& task, const std::vector<int>& marking) {
  for (size_t place : task.places) {
    if (place == task.exit_place) {
      continue;  // completion notification, not an executing/released job
    }
    if (place < marking.size() && marking[place] > 0) {
      return true;
    }
  }
  return false;
}

std::string FiringEdge::to_string() const {
  std::ostringstream oss;
  oss << "T" << transition_id << "@[" << firing_min << ", "
      << (firing_max == INF_TIME ? "inf" : std::to_string(firing_max)) << "]";
  return oss.str();
}

size_t MarkingHash::operator()(const std::vector<int>& marking) const {
  size_t seed = 0;
  std::hash<int> int_hash;
  for (int value : marking) {
    hash_combine(seed, int_hash(value));
  }
  return seed;
}

size_t hash_state_class(const StateClass& state) {
  size_t seed = 0;
  std::hash<int> int_hash;
  std::hash<size_t> size_hash;

  for (int value : state.marking) {
    hash_combine(seed, int_hash(value));
  }
  // The selected active set is part of the semantic state: two classes can
  // share marking/layout/zone but run different clocks. `suspended` is
  // included as well so that a layout-equivalent but scheduler-different class
  // never collides.
  hash_combine(seed, state.priority_enabled.size());
  for (size_t t : state.priority_enabled) {
    hash_combine(seed, size_hash(t));
  }
  hash_combine(seed, state.suspended.size());
  for (size_t t : state.suspended) {
    hash_combine(seed, size_hash(t));
  }
  for (const ClockVar& var : state.clock_vars) {
    hash_combine(seed, size_hash(static_cast<size_t>(var.kind)));
    hash_combine(seed, size_hash(var.transition));
    hash_combine(seed, size_hash(var.task));
  }
  // The release-clock map is derivable from clock_vars, but it is part of the
  // scheduling identity the rule reads; hash it explicitly so no future layout
  // change can make two different release/scheduling states collide.
  hash_combine(seed, state.release_clock_of_task.size());
  for (int index : state.release_clock_of_task) {
    hash_combine(seed, static_cast<size_t>(static_cast<long long>(index) + 1));
  }
  for (int value : state.zone.raw_matrix()) {
    hash_combine(seed, int_hash(value));
  }
  return seed;
}

}  // namespace state_class
