#include "analysis/reachability/scheduling/pcp.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "analysis/reachability/scheduling/policy_common.h"
#include "common/unsupported_class.h"

namespace state_class {

PcpFiringRule::PcpFiringRule(const petri::PTPN& net) {
  index_ = std::make_unique<detail::TaskLockTopology>();
  index_->build(net);
}

void PcpFiringRule::ensure_index(const petri::PTPN& net) const {
  if (!index_ || !index_->built_for(net)) {
    index_ = std::make_unique<detail::TaskLockTopology>();
    index_->build(net);
    cache_valid_ = false;
  }
}

std::optional<int> PcpFiringRule::lock_ceiling(const std::string& lock) const {
  if (!index_ || index_->net() == nullptr) {
    return std::nullopt;
  }
  return index_->lock_ceiling(lock);
}

const PcpFiringRule::DynamicState& PcpFiringRule::dynamic_state(
    const petri::PTPN& net, const petri::Marking& marking) const {
  ensure_index(net);

  if (cache_valid_ && cached_net_ == &net && cached_marking_ == marking) {
    return cached_dynamic_;
  }

  const detail::TaskLockTopology& topology = *index_;
  const std::vector<detail::TaskLockTopology::TaskRecord>& tasks = topology.tasks();
  const size_t task_count = tasks.size();

  DynamicState state;
  state.effective.resize(task_count, 0);
  for (size_t i = 0; i < task_count; ++i) {
    state.effective[i] = tasks[i].base_priority;
  }
  state.layout = topology.analyze_marking(marking);

  std::vector<std::pair<int, int>> wait_edges;

  // Direct blocking: the requested lock is held. The requester inherits from
  // the unique holder.
  for (const auto& [waiter, lock] : state.layout.blocked_requests) {
    const auto holder_it = state.layout.unique_holder.find(lock);
    if (holder_it == state.layout.unique_holder.end() || holder_it->second == waiter) {
      continue;
    }
    wait_edges.emplace_back(waiter, holder_it->second);
  }

  // Ceiling blocking: the requested lock is free but the requester's base
  // priority does not exceed the system ceiling. The deterministic PCP
  // instance blocks on the holder of the lexicographically smallest held lock
  // attaining that ceiling.
  for (const auto& [waiter, lock] : state.layout.free_requests) {
    (void)lock;
    const std::optional<int> ceiling = topology.system_ceiling_excluding(state.layout, waiter);
    if (!ceiling) {
      continue;  // no other task holds a lock: always admissible
    }
    if (static_cast<size_t>(waiter) >= tasks.size()) {
      continue;
    }
    if (tasks[static_cast<size_t>(waiter)].base_priority > *ceiling) {
      continue;  // admission threshold satisfied
    }
    int holder = -1;
    for (const auto& [held_lock, held_by] : state.layout.unique_holder) {
      if (held_by == waiter) {
        continue;  // own locks are excluded from the system ceiling
      }
      const std::optional<int> held_ceiling = topology.lock_ceiling(held_lock);
      if (!held_ceiling || *held_ceiling != *ceiling) {
        continue;
      }
      holder = held_by;
      break;  // std::map iteration is lexicographic by lock name
    }
    if (holder >= 0 && holder != waiter) {
      wait_edges.emplace_back(waiter, holder);
    }
  }

  detail::inherit_priorities(state.effective, wait_edges, task_count);

  cached_dynamic_ = std::move(state);
  cached_marking_ = marking;
  cached_net_ = &net;
  cache_valid_ = true;
  return cached_dynamic_;
}

int PcpFiringRule::effective_priority(const PolicyContext& context, size_t transition) const {
  ensure_index(context.net);
  const size_t num_transitions = index_->task_of_transition().size();
  if (transition < num_transitions) {
    const int task = index_->task_of_transition()[transition];
    if (task >= 0 && context.state != nullptr) {
      const DynamicState& state = dynamic_state(context.net, context.state->marking);
      if (static_cast<size_t>(task) < state.effective.size()) {
        return state.effective[static_cast<size_t>(task)];
      }
    }
  }
  // Control transitions and transitions outside every task chain keep their
  // statically encoded priority.
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool PcpFiringRule::admissible(const PolicyContext& context, size_t transition) const {
  ensure_index(context.net);
  const auto acquire_it = index_->acquire_transitions().find(transition);
  if (acquire_it == index_->acquire_transitions().end()) {
    return true;  // not a lock-acquire transition
  }
  if (context.state == nullptr) {
    return true;  // no marking: no ceiling information, no restriction
  }

  const petri::Marking& marking = context.state->marking;
  if (marking.size() != context.net.num_places()) {
    return true;  // defensive fallback: cannot reason about this marking
  }

  const int task = acquire_it->second.first;
  const size_t lock_index = acquire_it->second.second;
  const auto& task_record = index_->tasks()[static_cast<size_t>(task)];
  const detail::TaskLockTopology::LockBinding& binding = task_record.locks[lock_index];

  const DynamicState& state = dynamic_state(context.net, marking);

  // A lock held by another task already makes the acquire structurally
  // disabled. That is a direct block, not an admission decision.
  const auto holder_it = state.layout.unique_holder.find(binding.lock);
  if (holder_it != state.layout.unique_holder.end() && holder_it->second != task) {
    return true;
  }

  // The ceiling rule only applies to a structurally enabled acquire of a free
  // lock. With the lock place empty the transition cannot fire anyway.
  if (marking[binding.lock_place] == 0) {
    return true;
  }

  const std::optional<int> ceiling = index_->system_ceiling_excluding(state.layout, task);
  if (!ceiling) {
    return true;  // no other task holds a lock
  }
  return task_record.base_priority > *ceiling;
}

}  // namespace state_class
