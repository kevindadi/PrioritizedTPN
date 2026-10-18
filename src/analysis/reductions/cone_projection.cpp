#include "analysis/reductions/cone_projection.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace petri {
namespace {

// One projectable task: its chain endpoints are the only nodes the dependency
// search needs, because every arc between tasks is routed through a control
// transition (bridge, fork, join, consume, or a restart-preemption transition).
struct TaskDescriptor {
  std::string name;
  PlaceId entry{0};
  PlaceId exit{0};
  int core = -1;
  std::vector<std::string> locks;
  std::vector<NodeRef> chain;
};

// Task-level dependencies and the control nodes discovered on the paths that
// realise them. Places are vector offsets into PTPN::places(), transitions into
// PTPN::transitions(), so they can be filtered by the retained-node masks.
struct DependencyInfo {
  std::vector<size_t> predecessors;
  std::vector<size_t> successors;
  std::vector<size_t> control_places;
  std::vector<size_t> control_transitions;
};

void sort_unique(std::vector<size_t>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

class ConeBuilder {
 public:
  ConeBuilder(const PTPN& net, const std::string& focus)
      : net_(net),
        num_places_(net.num_places()),
        num_transitions_(net.num_transitions()),
        place_owner_(num_places_, -1),
        is_entry_place_(num_places_, 0),
        is_exit_place_(num_places_, 0),
        is_chain_transition_(num_transitions_, 0),
        consumers_(num_places_),
        producers_(num_places_) {
    build_task_index();
    const auto focus_it = task_by_name_.find(focus);
    if (focus_it == task_by_name_.end()) {
      throw std::invalid_argument(
          "project_cone: task '" + focus +
          "' has no lowering metadata (task_info/node_pn_map); using the full graph");
    }
    focus_ = focus_it->second;
    build_adjacency();
    build_place_owners();
  }

  PTPN build() {
    in_cone_.assign(tasks_.size(), 0);
    keep_place_.assign(num_places_, 0);
    keep_transition_.assign(num_transitions_, 0);
    dependency_cache_.assign(tasks_.size(), std::nullopt);

    std::deque<size_t> work;
    add_task(focus_, work);
    while (!work.empty()) {
      const size_t task_index = work.front();
      work.pop_front();
      expand_task(task_index, work);
    }

    // Retaining every place adjacent to a retained transition guarantees that
    // each retained transition keeps its complete arc set: lock/cpu resource
    // places, period places and control places are pulled in here.
    for (size_t t = 0; t < num_transitions_; ++t) {
      if (!keep_transition_[t]) {
        continue;
      }
      for (const auto& [place, weight] : net_.pre_arcs()[t]) {
        (void)weight;
        keep_place_[place] = 1;
      }
      for (const auto& [place, weight] : net_.post_arcs()[t]) {
        (void)weight;
        keep_place_[place] = 1;
      }
    }

    return materialize();
  }

 private:
  void build_task_index() {
    std::vector<std::string> names;
    names.reserve(net_.task_info().size());
    for (const auto& [name, info] : net_.task_info()) {
      (void)info;
      names.push_back(name);
    }
    // Deterministic order keeps projected node ids stable across runs and
    // therefore keeps metrics JSON reproducible.
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
      const auto chain_it = net_.node_pn_map().find(name);
      if (chain_it == net_.node_pn_map().end() || chain_it->second.size() < 2) {
        continue;
      }
      const std::vector<NodeRef>& chain = chain_it->second;
      if (!chain.front().is_place() || !chain.back().is_place()) {
        continue;
      }

      TaskDescriptor descriptor;
      descriptor.name = name;
      descriptor.entry = chain.front().as_place();
      descriptor.exit = chain.back().as_place();
      const auto info_it = net_.task_info().find(name);
      descriptor.core = info_it->second.core;
      descriptor.locks = info_it->second.locks;
      descriptor.chain = chain;

      task_by_name_.emplace(name, tasks_.size());
      tasks_.push_back(std::move(descriptor));
    }
  }

  void build_adjacency() {
    for (size_t t = 0; t < num_transitions_; ++t) {
      for (const auto& [place, weight] : net_.pre_arcs()[t]) {
        (void)weight;
        consumers_[place].push_back(t);
      }
      for (const auto& [place, weight] : net_.post_arcs()[t]) {
        (void)weight;
        producers_[place].push_back(t);
      }
    }
  }

  void build_place_owners() {
    for (size_t task_index = 0; task_index < tasks_.size(); ++task_index) {
      const TaskDescriptor& task = tasks_[task_index];
      if (place_owner_[task.entry.index()] < 0) {
        place_owner_[task.entry.index()] = static_cast<int>(task_index);
      }
      if (place_owner_[task.exit.index()] < 0) {
        place_owner_[task.exit.index()] = static_cast<int>(task_index);
      }
      is_entry_place_[task.entry.index()] = 1;
      is_exit_place_[task.exit.index()] = 1;
      for (const NodeRef& node : task.chain) {
        if (node.is_place()) {
          const size_t place = node.as_place().index();
          if (place_owner_[place] < 0) {
            place_owner_[place] = static_cast<int>(task_index);
          }
        } else {
          is_chain_transition_[node.as_transition().index()] = 1;
        }
      }
    }
  }

  // Computes the tasks directly connected to `task_index` through control flow
  // and the control nodes on those paths. The search only crosses transitions
  // that are not part of a task chain; task chains are opaque so a predecessor
  // is reached at its exit place and a successor at its entry place.
  const DependencyInfo& discover(size_t task_index) {
    std::optional<DependencyInfo>& cached = dependency_cache_[task_index];
    if (cached.has_value()) {
      return *cached;
    }

    DependencyInfo info;
    const TaskDescriptor& task = tasks_[task_index];

    {
      std::vector<char> seen_place(num_places_, 0);
      std::vector<char> seen_transition(num_transitions_, 0);
      std::deque<size_t> queue;
      const size_t start = task.exit.index();
      seen_place[start] = 1;
      queue.push_back(start);

      while (!queue.empty()) {
        const size_t place = queue.front();
        queue.pop_front();
        for (size_t transition : consumers_[place]) {
          if (is_chain_transition_[transition] || seen_transition[transition]) {
            continue;
          }
          seen_transition[transition] = 1;
          info.control_transitions.push_back(transition);

          for (const auto& [next_place, weight] : net_.post_arcs()[transition]) {
            (void)weight;
            if (seen_place[next_place]) {
              continue;
            }
            seen_place[next_place] = 1;
            if (is_entry_place_[next_place] || is_exit_place_[next_place]) {
              const int owner = place_owner_[next_place];
              if (owner >= 0) {
                info.successors.push_back(static_cast<size_t>(owner));
              }
              continue;
            }
            info.control_places.push_back(next_place);
            queue.push_back(next_place);
          }
        }
      }
    }

    {
      std::vector<char> seen_place(num_places_, 0);
      std::vector<char> seen_transition(num_transitions_, 0);
      std::deque<size_t> queue;
      const size_t start = task.entry.index();
      seen_place[start] = 1;
      queue.push_back(start);

      while (!queue.empty()) {
        const size_t place = queue.front();
        queue.pop_front();
        for (size_t transition : producers_[place]) {
          if (is_chain_transition_[transition] || seen_transition[transition]) {
            continue;
          }
          seen_transition[transition] = 1;
          info.control_transitions.push_back(transition);

          for (const auto& [previous_place, weight] : net_.pre_arcs()[transition]) {
            (void)weight;
            if (seen_place[previous_place]) {
              continue;
            }
            seen_place[previous_place] = 1;
            if (is_entry_place_[previous_place] || is_exit_place_[previous_place]) {
              const int owner = place_owner_[previous_place];
              if (owner >= 0) {
                info.predecessors.push_back(static_cast<size_t>(owner));
              }
              continue;
            }
            info.control_places.push_back(previous_place);
            queue.push_back(previous_place);
          }
        }
      }
    }

    sort_unique(info.predecessors);
    sort_unique(info.successors);
    sort_unique(info.control_places);
    sort_unique(info.control_transitions);
    cached = std::move(info);
    return *cached;
  }

  void add_task(size_t task_index, std::deque<size_t>& work) {
    if (in_cone_[task_index]) {
      return;
    }
    in_cone_[task_index] = 1;
    work.push_back(task_index);
  }

  void expand_task(size_t task_index, std::deque<size_t>& work) {
    const TaskDescriptor& task = tasks_[task_index];

    for (const NodeRef& node : task.chain) {
      if (node.is_place()) {
        keep_place_[node.as_place().index()] = 1;
      } else {
        keep_transition_[node.as_transition().index()] = 1;
      }
    }

    // Rule 2: tasks on the same real core compete for the CPU. The control core
    // (-1) is not scheduled and therefore does not form a competition group.
    if (task.core >= 0) {
      for (size_t other = 0; other < tasks_.size(); ++other) {
        if (tasks_[other].core == task.core) {
          add_task(other, work);
        }
      }
    }

    // Rule 3: transitive shared-lock closure.
    for (const std::string& lock : task.locks) {
      const auto lock_it = net_.lock_places().find(lock);
      if (lock_it != net_.lock_places().end()) {
        keep_place_[lock_it->second.index()] = 1;
      }
      for (size_t other = 0; other < tasks_.size(); ++other) {
        const auto& locks = tasks_[other].locks;
        if (std::find(locks.begin(), locks.end(), lock) != locks.end()) {
          add_task(other, work);
        }
      }
    }

    // Rule 4: dependency neighbours plus the control transitions between them.
    const DependencyInfo& dependencies = discover(task_index);
    for (size_t predecessor : dependencies.predecessors) {
      add_task(predecessor, work);
    }
    for (size_t successor : dependencies.successors) {
      add_task(successor, work);
    }
    for (size_t place : dependencies.control_places) {
      keep_place_[place] = 1;
    }
    for (size_t transition : dependencies.control_transitions) {
      keep_transition_[transition] = 1;
    }

    const auto periodic_it = net_.periodic_transitions().find(task.name);
    if (periodic_it != net_.periodic_transitions().end()) {
      keep_transition_[periodic_it->second.index()] = 1;
    }
  }

  PTPN materialize() {
    PTPN projected;
    projected.set_schedule_policy(net_.schedule_policy());
    std::vector<PlaceId> place_map(num_places_);
    std::vector<char> place_mapped(num_places_, 0);
    for (size_t p = 0; p < num_places_; ++p) {
      if (!keep_place_[p]) {
        continue;
      }
      const Place& place = net_.get_place(PlaceId{p});
      place_map[p] = projected.add_place(place.name, place.capacity, place.saturate);
      place_mapped[p] = 1;
    }

    std::vector<TransitionId> transition_map(num_transitions_);
    std::vector<char> transition_mapped(num_transitions_, 0);
    for (size_t t = 0; t < num_transitions_; ++t) {
      if (!keep_transition_[t]) {
        continue;
      }
      const Transition& transition = net_.get_transition(TransitionId{t});
      transition_map[t] =
          projected.add_transition(transition.name, transition.time_interval,
                                   transition.priority, transition.core, transition.suspendable);
      transition_mapped[t] = 1;
    }

    std::vector<ArcSpec> pre_arcs;
    std::vector<ArcSpec> post_arcs;
    for (size_t t = 0; t < num_transitions_; ++t) {
      if (!keep_transition_[t]) {
        continue;
      }
      for (const auto& [place, weight] : net_.pre_arcs()[t]) {
        if (place_mapped[place]) {
          pre_arcs.push_back({place_map[place], transition_map[t], weight});
        }
      }
      for (const auto& [place, weight] : net_.post_arcs()[t]) {
        if (place_mapped[place]) {
          post_arcs.push_back({place_map[place], transition_map[t], weight});
        }
      }
    }
    projected.set_arcs(pre_arcs, post_arcs);

    for (size_t p = 0; p < num_places_; ++p) {
      if (place_mapped[p]) {
        projected.set_initial_marking(place_map[p], net_.get_marking()[p]);
      }
    }

    for (size_t task_index = 0; task_index < tasks_.size(); ++task_index) {
      if (!in_cone_[task_index]) {
        continue;
      }
      const TaskDescriptor& task = tasks_[task_index];
      projected.set_task_info(task.name, net_.task_info().at(task.name));

      std::vector<NodeRef> chain;
      chain.reserve(task.chain.size());
      for (const NodeRef& node : task.chain) {
        if (node.is_place()) {
          const size_t place = node.as_place().index();
          chain.push_back(NodeRef::of(place_map[place]));
        } else {
          const size_t transition = node.as_transition().index();
          chain.push_back(NodeRef::of(transition_map[transition]));
        }
      }
      projected.set_task_chain(task.name, std::move(chain));
    }

    for (const auto& [name, span] : net_.node_start_end_map()) {
      if (!is_retained(span.first) || !is_retained(span.second)) {
        continue;
      }
      projected.set_node_span(name, remap(span.first, place_map, transition_map),
                              remap(span.second, place_map, transition_map));
    }

    for (const auto& [lock, place] : net_.lock_places()) {
      if (place_mapped[place.index()]) {
        projected.set_lock_place(lock, place_map[place.index()]);
      }
    }

    for (const auto& [task_name, transition] : net_.periodic_transitions()) {
      const auto task_it = task_by_name_.find(task_name);
      if (task_it != task_by_name_.end() && in_cone_[task_it->second] &&
          transition_mapped[transition.index()]) {
        projected.set_periodic_transition(task_name, transition_map[transition.index()]);
      }
    }

    for (const PlaceId place : net_.cpu_places()) {
      if (place_mapped[place.index()]) {
        projected.add_cpu_place(place_map[place.index()]);
      }
    }

    std::set<int> cores;
    for (size_t t = 0; t < num_transitions_; ++t) {
      if (keep_transition_[t] && net_.get_transition(TransitionId{t}).core >= 0) {
        cores.insert(net_.get_transition(TransitionId{t}).core);
      }
    }
    for (size_t task_index = 0; task_index < tasks_.size(); ++task_index) {
      if (in_cone_[task_index] && tasks_[task_index].core >= 0) {
        cores.insert(tasks_[task_index].core);
      }
    }
    for (int core : cores) {
      projected.set_core_parallelism(core, net_.parallelism_of_core(core));
    }

    return projected;
  }

  [[nodiscard]] bool is_retained(const NodeRef& node) const {
    return node.is_place() ? keep_place_[node.as_place().index()] != 0
                           : keep_transition_[node.as_transition().index()] != 0;
  }

  [[nodiscard]] NodeRef remap(const NodeRef& node, const std::vector<PlaceId>& place_map,
                              const std::vector<TransitionId>& transition_map) const {
    return node.is_place() ? NodeRef::of(place_map[node.as_place().index()])
                           : NodeRef::of(transition_map[node.as_transition().index()]);
  }

  const PTPN& net_;
  const size_t num_places_;
  const size_t num_transitions_;

  std::vector<TaskDescriptor> tasks_;
  std::unordered_map<std::string, size_t> task_by_name_;
  size_t focus_ = 0;

  std::vector<int> place_owner_;       // place -> task index owning the chain place
  std::vector<char> is_entry_place_;   // place is a task chain entry
  std::vector<char> is_exit_place_;    // place is a task chain exit
  std::vector<char> is_chain_transition_;
  std::vector<std::vector<size_t>> consumers_;  // place -> transitions reading it
  std::vector<std::vector<size_t>> producers_;  // place -> transitions producing it

  std::vector<std::optional<DependencyInfo>> dependency_cache_;
  std::vector<char> in_cone_;
  std::vector<char> keep_place_;
  std::vector<char> keep_transition_;
};

}  // namespace

PTPN project_cone(const PTPN& net, const std::string& task) {
  return ConeBuilder(net, task).build();
}

}  // namespace petri
