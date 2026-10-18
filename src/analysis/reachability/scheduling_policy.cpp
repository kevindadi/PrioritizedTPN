#include "analysis/reachability/scheduling_policy.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>

#include "analysis/reachability/state_class.h"

namespace state_class {

namespace {

// Mirrors converter::detail::TaskChainLayout. The analysis layer deliberately
// does not include lowering headers. Lock i is acquired by the transition at
// position `5 + 4*i` and released by the transition at
// `chain.size() - 4 - 2*i`.
bool valid_lock_binding(const std::vector<petri::NodeRef>& chain, size_t lock_index) {
  const size_t minimum =
      detail::TaskLockTopology::kLockReleaseSuffix + 2 * lock_index + 1;
  if (chain.size() < minimum) {
    return false;  // also prevents underflow in the release calculation
  }

  const size_t acquire =
      detail::TaskLockTopology::kLockAcquireBase +
      detail::TaskLockTopology::kLockStride * lock_index;
  const size_t release =
      chain.size() - detail::TaskLockTopology::kLockReleaseSuffix - 2 * lock_index;
  if (acquire == 0 || acquire + 1 >= chain.size() || release >= chain.size()) {
    return false;
  }
  // The ownership range [acquire+1, release-1] must be non-empty.
  if (release <= acquire + 1) {
    return false;
  }
  return chain[acquire].is_transition() && chain[acquire - 1].is_place() &&
         chain[acquire + 1].is_place() && chain[release].is_transition() &&
         chain[release - 1].is_place();
}

// Least fixpoint of eff[holder] = max(eff[holder], eff[waiter]) over the wait
// edges. The update is monotone and bounded by the largest base priority, so at
// most task_count rounds are needed; a cycle simply saturates at the maximum.
void inherit_priorities(std::vector<int>& effective,
                        const std::vector<std::pair<int, int>>& edges,
                        size_t task_count) {
  for (size_t round = 0; round < task_count; ++round) {
    bool changed = false;
    for (const auto& [waiter, holder] : edges) {
      if (waiter < 0 || holder < 0) {
        continue;
      }
      const size_t w = static_cast<size_t>(waiter);
      const size_t h = static_cast<size_t>(holder);
      if (w >= effective.size() || h >= effective.size()) {
        continue;
      }
      if (effective[h] < effective[w]) {
        effective[h] = effective[w];
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared task-chain / lock topology
// ---------------------------------------------------------------------------

void detail::TaskLockTopology::build(const petri::PTPN& net) {
  net_ = &net;
  tasks_.clear();
  task_of_transition_.assign(net.num_transitions(), -1);
  acquire_transitions_.clear();
  lock_ceilings_.clear();

  // Lock ceilings are defined by TaskInfo alone: every task whose
  // TaskInfo::locks contains the lock contributes its base priority,
  // independent of the chain layout or of which task currently holds it.
  for (const auto& entry : net.task_info()) {
    const petri::TaskInfo& info = entry.second;
    for (const std::string& lock : info.locks) {
      const auto it = lock_ceilings_.find(lock);
      if (it == lock_ceilings_.end()) {
        lock_ceilings_.emplace(lock, info.priority);
      } else {
        it->second = std::max(it->second, info.priority);
      }
    }
  }

  // Deterministic task order: unordered_map iteration order must not leak into
  // the fixpoint iteration (the result is order-independent, but tests and
  // diagnostics stay stable).
  std::vector<std::string> names;
  names.reserve(net.node_pn_map().size());
  for (const auto& entry : net.node_pn_map()) {
    names.push_back(entry.first);
  }
  std::sort(names.begin(), names.end());

  for (const std::string& name : names) {
    const auto chain_it = net.node_pn_map().find(name);
    if (chain_it == net.node_pn_map().end() || chain_it->second.empty()) {
      continue;
    }
    const std::vector<petri::NodeRef>& chain = chain_it->second;

    TaskRecord task;
    task.name = name;
    task.chain = chain;

    const auto info_it = net.task_info().find(name);
    const petri::TaskInfo* info =
        info_it == net.task_info().end() ? nullptr : &info_it->second;
    bool have_priority = false;
    if (info != nullptr) {
      task.base_priority = info->priority;
      have_priority = true;
    }

    for (size_t position = 0; position < chain.size(); ++position) {
      if (chain[position].is_place()) {
        const size_t place = chain[position].as_place().index();
        task.chain_places.push_back(place);
        task.position_of_place[place] = position;
      } else {
        task.chain_transitions.push_back(chain[position].as_transition().index());
        if (!have_priority) {
          task.base_priority =
              net.get_transition(chain[position].as_transition()).priority;
          have_priority = true;
        }
      }
    }
    if (!have_priority) {
      task.base_priority = 0;
    }

    const int ordinal = static_cast<int>(tasks_.size());
    if (info != nullptr) {
      for (size_t lock_index = 0; lock_index < info->locks.size(); ++lock_index) {
        if (!valid_lock_binding(chain, lock_index)) {
          // A malformed/short chain: no ownership inference through this lock.
          continue;
        }
        const auto lock_it = net.lock_places().find(info->locks[lock_index]);
        if (lock_it == net.lock_places().end()) {
          // Without the shared lock place the ownership cross-check is
          // impossible; stay conservative and ignore this binding.
          continue;
        }
        LockBinding binding;
        binding.lock = info->locks[lock_index];
        binding.lock_place = lock_it->second.index();
        binding.acquire_position =
            kLockAcquireBase + kLockStride * lock_index;
        binding.acquire_transition =
            chain[binding.acquire_position].as_transition().index();
        binding.release_position =
            chain.size() - kLockReleaseSuffix - 2 * lock_index;
        acquire_transitions_[binding.acquire_transition] = {ordinal, lock_index};
        task.locks.push_back(std::move(binding));
      }
    }

    for (size_t transition : task.chain_transitions) {
      if (transition < task_of_transition_.size()) {
        task_of_transition_[transition] = ordinal;
      }
    }
    tasks_.push_back(std::move(task));
  }
}

detail::TaskLockTopology::MarkingState detail::TaskLockTopology::analyze_marking(
    const petri::Marking& marking) const {
  MarkingState state;
  const size_t task_count = tasks_.size();
  state.supported.assign(task_count, false);
  state.token_position.assign(task_count, kNoChainPosition);

  // A marking of the wrong size cannot be reasoned about; every task falls
  // back to its base priority and no ownership is inferred.
  if (net_ == nullptr || marking.size() != net_->num_places()) {
    return state;
  }

  // --- Unique chain-token position per task. ----------------------------
  // The token moves through every chain place, including the lock-hold
  // places. Exactly one token in the chain is the supported state; zero or
  // multiple tokens are a defensive fallback (no inheritance for that task).
  for (size_t i = 0; i < task_count; ++i) {
    const TaskRecord& task = tasks_[i];
    int tokens = 0;
    size_t position = kNoChainPosition;
    for (size_t place : task.chain_places) {
      const int count = marking[place];
      if (count <= 0) {
        continue;
      }
      tokens += count;
      if (tokens == 1) {
        const auto position_it = task.position_of_place.find(place);
        if (position_it != task.position_of_place.end()) {
          position = position_it->second;
        }
      }
      if (tokens > 1) {
        break;
      }
    }
    if (tokens == 1 && position != kNoChainPosition) {
      state.supported[i] = true;
      state.token_position[i] = position;
    }
  }

  // --- Held locks: chain-position range ownership. ----------------------
  // Lock i is held while the token lies in [acquire+1, release-1], i.e. from
  // the hold place through the release transition's pre-place. This is exact
  // for a single token even after it leaves the hold place. The shared lock
  // place must be empty; a non-empty place contradicts ownership and suppresses
  // the inference.
  std::map<std::string, std::vector<int>> claimants;
  for (size_t i = 0; i < task_count; ++i) {
    if (!state.supported[i]) {
      continue;
    }
    const size_t position = state.token_position[i];
    for (const LockBinding& binding : tasks_[i].locks) {
      if (position < binding.acquire_position + 1 ||
          position > binding.release_position - 1) {
        continue;
      }
      if (marking[binding.lock_place] != 0) {
        continue;  // inconsistent marking: no holder inference
      }
      claimants[binding.lock].push_back(static_cast<int>(i));
    }
  }
  for (auto& [lock, tasks] : claimants) {
    std::sort(tasks.begin(), tasks.end());
    tasks.erase(std::unique(tasks.begin(), tasks.end()), tasks.end());
    // Two tasks claiming the same lock make the marking inconsistent:
    // ownership through this lock is disabled.
    if (tasks.size() == 1) {
      state.unique_holder[lock] = tasks.front();
    }
  }

  // --- Requests from the marking. ---------------------------------------
  // A task requests lock i when its unique token sits on the acquire
  // transition's pre-place (position acquire-1). The request is a direct block
  // when the acquire transition is not structurally enabled, and a free
  // request otherwise. A lock-blocked acquire is absent from struct_enabled,
  // so this must be checked against the marking.
  for (size_t i = 0; i < task_count; ++i) {
    if (!state.supported[i]) {
      continue;
    }
    const size_t position = state.token_position[i];
    for (const LockBinding& binding : tasks_[i].locks) {
      if (position != binding.acquire_position - 1) {
        continue;
      }
      if (petri::PTPN::is_enabled(marking, *net_,
                                  petri::TransitionId{binding.acquire_transition})) {
        state.free_requests.emplace_back(static_cast<int>(i), binding.lock);
      } else {
        state.blocked_requests.emplace_back(static_cast<int>(i), binding.lock);
      }
      break;  // A single token occupies at most one acquire pre-place.
    }
  }

  return state;
}

std::optional<int> detail::TaskLockTopology::lock_ceiling(
    const std::string& lock) const {
  const auto it = lock_ceilings_.find(lock);
  if (it == lock_ceilings_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<int> detail::TaskLockTopology::system_ceiling_excluding(
    const MarkingState& state, int task) const {
  std::optional<int> ceiling;
  for (const auto& [lock, holder] : state.unique_holder) {
    if (holder == task) {
      continue;
    }
    const auto ceiling_it = lock_ceilings_.find(lock);
    if (ceiling_it == lock_ceilings_.end()) {
      continue;
    }
    if (!ceiling || ceiling_it->second > *ceiling) {
      ceiling = ceiling_it->second;
    }
  }
  return ceiling;
}

std::optional<int> detail::TaskLockTopology::system_ceiling(
    const MarkingState& state) const {
  std::optional<int> ceiling;
  for (const auto& [lock, holder] : state.unique_holder) {
    (void)holder;
    const auto ceiling_it = lock_ceilings_.find(lock);
    if (ceiling_it == lock_ceilings_.end()) {
      continue;
    }
    if (!ceiling || ceiling_it->second > *ceiling) {
      ceiling = ceiling_it->second;
    }
  }
  return ceiling;
}

// ---------------------------------------------------------------------------
// Static rule
// ---------------------------------------------------------------------------

bool FiringRule::admissible(const PolicyContext& /*context*/,
                            size_t /*transition*/) const {
  return true;
}

int StaticPriorityRule::effective_priority(const PolicyContext& context,
                                           size_t transition) const {
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool StaticPriorityRule::admissible(const PolicyContext& /*context*/,
                                    size_t /*transition*/) const {
  return true;
}

// ---------------------------------------------------------------------------
// PIP
// ---------------------------------------------------------------------------

PipFiringRule::PipFiringRule(const petri::PTPN& net) {
  index_ = std::make_unique<detail::TaskLockTopology>();
  index_->build(net);
}

void PipFiringRule::ensure_index(const petri::PTPN& net) const {
  if (!index_ || !index_->built_for(net)) {
    index_ = std::make_unique<detail::TaskLockTopology>();
    index_->build(net);
    cache_valid_ = false;
  }
}

const std::vector<int>& PipFiringRule::effective_priorities(
    const petri::PTPN& net, const petri::Marking& marking) const {
  ensure_index(net);

  if (cache_valid_ && cached_net_ == &net && cached_marking_ == marking) {
    return cached_effective_;
  }

  const detail::TaskLockTopology& topology = *index_;
  const size_t task_count = topology.tasks().size();
  std::vector<int> effective(task_count, 0);
  for (size_t i = 0; i < task_count; ++i) {
    effective[i] = topology.tasks()[i].base_priority;
  }

  const detail::TaskLockTopology::MarkingState layout =
      topology.analyze_marking(marking);
  std::vector<std::pair<int, int>> wait_edges;
  for (const auto& [waiter, lock] : layout.blocked_requests) {
    const auto holder_it = layout.unique_holder.find(lock);
    if (holder_it == layout.unique_holder.end() ||
        holder_it->second == waiter) {
      continue;
    }
    wait_edges.emplace_back(waiter, holder_it->second);
  }
  inherit_priorities(effective, wait_edges, task_count);

  cached_effective_ = std::move(effective);
  cached_marking_ = marking;
  cached_net_ = &net;
  cache_valid_ = true;
  return cached_effective_;
}

int PipFiringRule::effective_priority(const PolicyContext& context,
                                      size_t transition) const {
  ensure_index(context.net);
  const size_t num_transitions = index_->task_of_transition().size();
  if (transition < num_transitions) {
    const int task = index_->task_of_transition()[transition];
    if (task >= 0 && context.state != nullptr) {
      const std::vector<int>& effective =
          effective_priorities(context.net, context.state->marking);
      if (static_cast<size_t>(task) < effective.size()) {
        return effective[static_cast<size_t>(task)];
      }
    }
  }
  // Control transitions and transitions outside every task chain keep their
  // statically encoded priority.
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool PipFiringRule::admissible(const PolicyContext& /*context*/,
                               size_t /*transition*/) const {
  // PIP only changes effective priorities; it never blocks a transition.
  return true;
}

// ---------------------------------------------------------------------------
// Policy -> rule mapping
// ---------------------------------------------------------------------------

std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy) {
  switch (policy) {
    case SchedulePolicy::FIXED:
    case SchedulePolicy::FIXED_PRIOR_WITH_RESUME:
    case SchedulePolicy::RM:
    case SchedulePolicy::DM:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::RESUME);
    case SchedulePolicy::PIP:
      // Net-aware behavior is installed lazily from PolicyContext::net.
      return std::make_unique<PipFiringRule>();
    case SchedulePolicy::FIXED_PRIOR_WITH_RESTART:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::RESTART);
    case SchedulePolicy::PCP:
    case SchedulePolicy::SRP:
    case SchedulePolicy::EDF:
    case SchedulePolicy::LLF:
    case SchedulePolicy::FIFO:
    case SchedulePolicy::UNKNOWN:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::NON_PREEMPTIVE);
  }
  return std::make_unique<StaticPriorityRule>(PreemptionMode::NON_PREEMPTIVE);
}

std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy,
                                             const petri::PTPN& net) {
  if (policy == SchedulePolicy::PIP) {
    return std::make_unique<PipFiringRule>(net);
  }
  return make_firing_rule(policy);
}

}  // namespace state_class
