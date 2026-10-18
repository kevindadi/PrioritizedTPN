#include "analysis/reachability/scheduling_policy.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "analysis/reachability/scheduling.h"
#include "analysis/reachability/state_class.h"

namespace state_class {

namespace {

// Mirrors converter::detail::TaskChainLayout. The analysis layer deliberately
// does not include lowering headers. Lock i is acquired by the transition at
// position `5 + 4*i` and released by the transition at
// `chain.size() - 4 - 2*i`.
bool valid_lock_binding(const std::vector<petri::NodeRef>& chain, size_t lock_index) {
  const size_t minimum = detail::TaskLockTopology::kLockReleaseSuffix + 2 * lock_index + 1;
  if (chain.size() < minimum) {
    return false;  // also prevents underflow in the release calculation
  }

  const size_t acquire = detail::TaskLockTopology::kLockAcquireBase +
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
void inherit_priorities(std::vector<int>& effective, const std::vector<std::pair<int, int>>& edges,
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
    const petri::TaskInfo* info = info_it == net.task_info().end() ? nullptr : &info_it->second;
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
          task.base_priority = net.get_transition(chain[position].as_transition()).priority;
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
        binding.acquire_position = kLockAcquireBase + kLockStride * lock_index;
        binding.acquire_transition = chain[binding.acquire_position].as_transition().index();
        binding.release_position = chain.size() - kLockReleaseSuffix - 2 * lock_index;
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
      if (position < binding.acquire_position + 1 || position > binding.release_position - 1) {
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

std::optional<int> detail::TaskLockTopology::lock_ceiling(const std::string& lock) const {
  const auto it = lock_ceilings_.find(lock);
  if (it == lock_ceilings_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<int> detail::TaskLockTopology::system_ceiling_excluding(const MarkingState& state,
                                                                      int task) const {
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

std::optional<int> detail::TaskLockTopology::system_ceiling(const MarkingState& state) const {
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

bool FiringRule::admissible(const PolicyContext& /*context*/, size_t /*transition*/) const {
  return true;
}

std::vector<PolicyBranch> FiringRule::branches(const TransitionSet& struct_enabled,
                                               const petri::PTPN& net,
                                               const PolicyContext& context) const {
  std::vector<TransitionSet> actives =
      Scheduling::priority_branches(struct_enabled, net, *this, context);
  std::vector<PolicyBranch> branches;
  branches.reserve(actives.size());
  for (TransitionSet& active : actives) {
    branches.push_back(PolicyBranch{std::move(active), {}});
  }
  return branches;
}

std::vector<PolicyBranch> combine_core_choices(
    const TransitionSet& control, const std::map<int, std::vector<CoreChoice>>& by_core) {
  // Each prefix holds the choices made so far; control transitions are added
  // to every branch once at the end.
  std::vector<PolicyBranch> combined(1);
  for (const auto& [core, choices] : by_core) {
    if (choices.empty()) {
      throw std::invalid_argument("combine_core_choices: core " + std::to_string(core) +
                                  " has no feasible scheduling choice");
    }
    std::vector<PolicyBranch> next;
    next.reserve(combined.size() * choices.size());
    for (const PolicyBranch& prefix : combined) {
      for (const CoreChoice& choice : choices) {
        PolicyBranch branch;
        branch.active = prefix.active;
        branch.active.insert(branch.active.end(), choice.active.begin(), choice.active.end());
        branch.constraints = prefix.constraints;
        branch.constraints.insert(branch.constraints.end(), choice.constraints.begin(),
                                  choice.constraints.end());
        next.push_back(std::move(branch));
      }
    }
    combined = std::move(next);
  }

  for (PolicyBranch& branch : combined) {
    branch.active.insert(branch.active.end(), control.begin(), control.end());
    std::sort(branch.active.begin(), branch.active.end());
  }

  // Small branch counts: an order-preserving pairwise deduplication keeps the
  // enumeration deterministic without requiring an ordering on PolicyBranch.
  std::vector<PolicyBranch> unique;
  unique.reserve(combined.size());
  for (PolicyBranch& branch : combined) {
    if (std::find(unique.begin(), unique.end(), branch) == unique.end()) {
      unique.push_back(std::move(branch));
    }
  }
  return unique;
}

int StaticPriorityRule::effective_priority(const PolicyContext& context, size_t transition) const {
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool StaticPriorityRule::admissible(const PolicyContext& /*context*/, size_t /*transition*/) const {
  return true;
}

// ---------------------------------------------------------------------------
// EDF
// ---------------------------------------------------------------------------

namespace {

std::string task_name_or_ordinal(const std::vector<TaskChain>& tasks, size_t ordinal) {
  return ordinal < tasks.size() ? tasks[ordinal].name : "#" + std::to_string(ordinal);
}

// Task-chain index shared by the per-job-clock policies (EDF, FIFO and
// LLF-ED). They require the same one-job, no-lock lowering with statically
// partitioned tasks: each task chain carries at most one in-flight job and all
// of its transitions run on the task's declared real core. Every policy
// schedules each real core independently; tasks never migrate.
//
// `options` selects the policy-specific extra requirements: EDF/LLF need a
// positive deadline, LLF additionally needs deterministic execution segments
// plus a TaskInfo::wcet equal to their sum, and FIFO derives the supported core
// range from its structural CPU places (falling back to the TaskInfo cores for
// nets without a CPU place).
struct ReleaseClassOptions {
  const char* label = "EDF";
  bool require_deadline = true;
  bool use_cpu_places = false;
  bool require_deterministic_segments = false;
  bool require_single_core = false;
};

struct ReleaseTaskIndex {
  std::vector<TaskChain> tasks;
  std::vector<int> task_of_transition;
  std::vector<int> task_of_periodic_transition;
};

ReleaseTaskIndex build_release_task_index(const petri::PTPN& net,
                                          const ReleaseClassOptions& options) {
  const std::string name = options.label;
  ReleaseTaskIndex index;
  index.tasks = build_task_chains(net);
  if (index.tasks.empty()) {
    throw std::invalid_argument(name +
                                " supported class requires TDG2PN-lowered task metadata "
                                "(task_info/node_pn_map); this net has no task chains");
  }
  index.task_of_transition.assign(net.num_transitions(), -1);
  index.task_of_periodic_transition.assign(net.num_transitions(), -1);

  if (!net.lock_places().empty()) {
    throw std::invalid_argument(name +
                                " supported class does not include locks, but the net declares "
                                "shared lock resources");
  }

  // Supported core range. All three job-clock policies are statically
  // partitioned. Resume policies (EDF/LLF) derive the range from the TaskInfo
  // cores: max(TaskInfo.core) + 1. FIFO has a structural CPU place per real
  // core, so its range is the number of those places; a hand-built FIFO net
  // without CPU places takes the marking-based path and uses the TaskInfo cores
  // instead.
  size_t num_cores = 0;
  if (options.use_cpu_places && !net.cpu_places().empty()) {
    num_cores = net.cpu_places().size();
    for (const petri::PlaceId cpu : net.cpu_places()) {
      const petri::Place& place = net.get_place(cpu);
      if (place.capacity != 1) {
        throw std::invalid_argument(name +
                                    " requires cores_per_cpu == 1 (one running job per real core), "
                                    "but '" +
                                    place.name + "' has capacity " +
                                    std::to_string(place.capacity));
      }
    }
  } else {
    int max_core = -1;
    for (const auto& entry : net.task_info()) {
      max_core = std::max(max_core, entry.second.core);
    }
    num_cores = max_core >= 0 ? static_cast<size_t>(max_core) + 1 : 0;
  }

  if (options.require_single_core && num_cores > 1) {
    throw std::invalid_argument(name + " supported class is single-core, but the net has " +
                                std::to_string(num_cores) + " real cores");
  }

  if (num_cores < 1) {
    throw std::invalid_argument(name + " requires num_cpus >= 1");
  }

  const std::vector<int>& initial = net.get_marking();
  for (size_t ordinal = 0; ordinal < index.tasks.size(); ++ordinal) {
    const TaskChain& task = index.tasks[ordinal];
    const auto info_it = net.task_info().find(task.name);
    if (info_it == net.task_info().end()) {
      throw std::invalid_argument(name + " task '" + task.name + "' has no TaskInfo metadata");
    }
    const int task_core = info_it->second.core;
    if (task_core < 0 || static_cast<size_t>(task_core) >= num_cores) {
      throw std::invalid_argument(name + " task '" + task.name + "' is bound to core " +
                                  std::to_string(task_core) + " outside [0, " +
                                  std::to_string(num_cores) + ")");
    }
    if (!info_it->second.locks.empty()) {
      throw std::invalid_argument(name + " supported class does not include locks; task '" +
                                  task.name + "' declares locks");
    }
    if (options.require_deadline && info_it->second.deadline <= 0) {
      throw std::invalid_argument(name + " requires a positive absolute deadline for task '" +
                                  task.name + "'");
    }

    // Standard no-lock lowering chain: entry, get_core, ready, then alternating
    // exec/place pairs, ending in the exit place.
    if (task.chain.size() < 5 || task.chain.size() % 2 == 0 || !task.chain.front().is_place() ||
        !task.chain.back().is_place()) {
      throw std::invalid_argument(
          name + " supported class requires the standard lowered task chain for '" + task.name +
          "' (alternating place/transition, at least 5 nodes)");
    }
    for (size_t position = 0; position < task.chain.size(); ++position) {
      const bool want_place = position % 2 == 0;
      if (task.chain[position].is_place() != want_place) {
        throw std::invalid_argument(
            name + " supported class requires the standard lowered task chain for '" + task.name +
            "' (chain positions must alternate place/transition)");
      }
    }
    if (task.entry_place == NO_TRANSITION || task.exit_place == NO_TRANSITION) {
      throw std::invalid_argument(name + " task '" + task.name + "' has no entry/exit place");
    }

    if (options.require_deterministic_segments) {
      // Position 1 is get_core (a zero-time dispatch step); positions 3, 5, ...
      // are the deterministic execution segments.
      const auto& dispatch = net.get_transition(petri::TransitionId{task.transitions.front()});
      if (dispatch.time_interval.left_open || dispatch.time_interval.right_open ||
          dispatch.time_interval.earliest != 0 || dispatch.time_interval.latest != 0) {
        throw std::invalid_argument(name +
                                    " requires a zero-time get_core dispatch step for task '" +
                                    task.name + "' (interval [0,0])");
      }
      long long wcet_sum = 0;
      for (size_t position = 1; position < task.transitions.size(); ++position) {
        const auto& segment = net.get_transition(petri::TransitionId{task.transitions[position]});
        if (segment.time_interval.left_open || segment.time_interval.right_open ||
            segment.time_interval.earliest != segment.time_interval.latest ||
            segment.time_interval.earliest < 0) {
          throw std::invalid_argument(name +
                                      " supports only deterministic execution segments "
                                      "(earliest == latest); task '" +
                                      task.name +
                                      "' has a non-deterministic or open interval on T" +
                                      std::to_string(task.transitions[position]));
        }
        wcet_sum += segment.time_interval.latest;
      }
      if (wcet_sum > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(name + " worst-case execution time for task '" + task.name +
                                    "' does not fit in an int");
      }
      if (info_it->second.wcet < 0 || static_cast<long long>(info_it->second.wcet) != wcet_sum) {
        throw std::invalid_argument(
            name +
            " requires TaskInfo::wcet to equal the sum of the deterministic "
            "execution-segment upper bounds for task '" +
            task.name + "' (metadata says " + std::to_string(info_it->second.wcet) + ", net says " +
            std::to_string(wcet_sum) + ")");
      }
      const long long initial_chi =
          static_cast<long long>(info_it->second.wcet) - info_it->second.deadline;
      if (initial_chi < std::numeric_limits<int>::min() ||
          initial_chi > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(name +
                                    " initial laxity chi = wcet - deadline "
                                    "does not fit in an int for task '" +
                                    task.name + "'");
      }
    }

    // The supported class fixes task_place_capacity == 1. A release that fires
    // while the job waits at the entry is clamped by the saturating place; a
    // release that fires mid-chain is rejected later by the chain-token guard
    // instead of being queued.
    for (size_t place_index : task.places) {
      if (place_index >= net.num_places()) {
        throw std::invalid_argument(name + " task '" + task.name +
                                    "' references an out-of-range place");
      }
      const auto& place = net.get_place(petri::PlaceId{place_index});
      if (place.capacity != 1 || !place.saturate) {
        throw std::invalid_argument(
            name + " requires task_place_capacity == 1 with saturating task places; task '" +
            task.name + "' uses a place with capacity " + std::to_string(place.capacity) +
            (place.saturate ? "" : " (non-saturating)"));
      }
    }

    for (size_t transition : task.transitions) {
      if (transition >= net.num_transitions()) {
        throw std::invalid_argument(name + " task '" + task.name +
                                    "' references an out-of-range transition");
      }
      const auto& lowered = net.get_transition(petri::TransitionId{transition});
      if (lowered.core != task_core) {
        throw std::invalid_argument(
            name + " task '" + task.name + "' has a chain transition T" +
            std::to_string(transition) + " on core " + std::to_string(lowered.core) +
            " (TaskInfo binds the task to core " + std::to_string(task_core) + ")");
      }
      if (index.task_of_transition[transition] >= 0) {
        throw std::invalid_argument(name + " task chains share transition T" +
                                    std::to_string(transition) +
                                    "; the standard lowering gives every task its own chain");
      }
      index.task_of_transition[transition] = static_cast<int>(ordinal);
    }

    const bool has_periodic = task.periodic_transition != NO_TRANSITION;
    if (has_periodic) {
      if (task.periodic_transition >= net.num_transitions()) {
        throw std::invalid_argument(name + " task '" + task.name +
                                    "' has an out-of-range periodic release transition");
      }
      const auto& release = net.get_transition(petri::TransitionId{task.periodic_transition});
      if (release.core >= 0) {
        throw std::invalid_argument(name + " periodic release for task '" + task.name +
                                    "' must be a control transition (core < 0), got core " +
                                    std::to_string(release.core));
      }
      if (release.time_interval.left_open || release.time_interval.right_open ||
          release.time_interval.earliest != release.time_interval.latest ||
          release.time_interval.latest <= 0) {
        throw std::invalid_argument(
            name + " supported class requires an exact positive release period for task '" +
            task.name + "' (a _fire transition with interval [P,P], P > 0)");
      }
      if (task.period <= 0) {
        throw std::invalid_argument(name + " periodic task '" + task.name +
                                    "' has no positive TaskInfo::period");
      }
      if (index.task_of_periodic_transition[task.periodic_transition] >= 0) {
        throw std::invalid_argument(name + " periodic release transition T" +
                                    std::to_string(task.periodic_transition) +
                                    " is shared by multiple tasks");
      }
      index.task_of_periodic_transition[task.periodic_transition] = static_cast<int>(ordinal);
    } else if (task.period > 0) {
      throw std::invalid_argument(
          name + " task '" + task.name +
          "' declares a period but the net has no periodic release transition");
    }

    const size_t chain_tokens = task_chain_token_count(task, initial);
    if (chain_tokens > 1) {
      throw std::invalid_argument(name + " supports at most one initial job per task; task '" +
                                  task.name + "' starts " + std::to_string(chain_tokens) +
                                  " tokens");
    }
    if (chain_tokens == 1 && initial[task.entry_place] <= 0) {
      throw std::invalid_argument(
          name + " initial job for task '" + task.name +
          "' must be at the task entry (one-shot start at time 0), not mid-chain");
    }
    if (!has_periodic && chain_tokens == 0) {
      throw std::invalid_argument(
          name + " task '" + task.name +
          "' is neither periodic (released by _fire) nor started at time 0");
    }
  }

  // Every real-core transition must belong to exactly one task chain. Control
  // transitions (core < 0) are outside the CPU scheduler.
  for (size_t transition = 0; transition < net.num_transitions(); ++transition) {
    const int core = net.get_transition(petri::TransitionId{transition}).core;
    if (core >= 0 && index.task_of_transition[transition] < 0) {
      throw std::invalid_argument(
          name + " supported class does not include real-core transition T" +
          std::to_string(transition) + " outside a task chain (fork/join or hand-built net)");
    }
  }

  // Only the task's own periodic release may put a token into its entry place
  // from a control transition. Anything else (a TDG task-to-task bridge, a
  // fork/join release, a hand-built releaser) is a dependency-driven release,
  // which is outside the independent-task supported class.
  for (size_t transition = 0; transition < net.num_transitions(); ++transition) {
    if (net.get_transition(petri::TransitionId{transition}).core >= 0) {
      continue;  // chain transitions are handled by the ownership check above
    }
    const int periodic_task = index.task_of_periodic_transition[transition];
    for (const auto& [place, weight] : net.post_arcs()[transition]) {
      (void)weight;
      for (size_t ordinal = 0; ordinal < index.tasks.size(); ++ordinal) {
        if (place != index.tasks[ordinal].entry_place) {
          continue;
        }
        if (periodic_task == static_cast<int>(ordinal)) {
          continue;  // the task's own _fire release
        }
        throw std::invalid_argument(
            name +
            " supported class does not include dependency-driven releases: control "
            "transition T" +
            std::to_string(transition) + " releases task '" + index.tasks[ordinal].name +
            "' without being its periodic _fire transition");
      }
    }
  }

  return index;
}

}  // namespace

EdfFiringRule::EdfFiringRule(const petri::PTPN& net) {
  ensure_index(net);
}

void EdfFiringRule::ensure_index(const petri::PTPN& net) const {
  if (net_ == &net) {
    return;
  }

  ReleaseClassOptions options;
  options.label = "EDF";
  options.require_deadline = true;
  // Multi-core EDF is statically partitioned: the supported core range comes
  // from the TaskInfo cores and each core is scheduled independently below.
  ReleaseTaskIndex index = build_release_task_index(net, options);
  tasks_ = std::move(index.tasks);
  task_of_transition_ = std::move(index.task_of_transition);
  task_of_periodic_transition_ = std::move(index.task_of_periodic_transition);
  net_ = &net;
}

int EdfFiringRule::effective_priority(const PolicyContext& /*context*/,
                                      size_t /*transition*/) const {
  throw std::logic_error(
      "EDF does not have a scalar static priority; scheduling is represented by "
      "release clocks and deadline-order branches");
}

std::vector<PolicyBranch> EdfFiringRule::branches(const TransitionSet& struct_enabled,
                                                  const petri::PTPN& net,
                                                  const PolicyContext& context) const {
  ensure_index(net);
  if (context.state == nullptr) {
    throw std::invalid_argument(
        "EDF branch selection requires a prepared state class with release clocks");
  }
  const StateClass& state = *context.state;
  if (state.marking.size() != net.num_places()) {
    throw std::invalid_argument("EDF state marking does not match the net");
  }
  if (state.release_clock_of_task.size() != tasks_.size()) {
    throw std::invalid_argument("EDF state does not carry the release-clock layout for every task");
  }

  TransitionSet control;
  std::vector<TransitionSet> enabled_per_task(tasks_.size());
  for (size_t transition : struct_enabled) {
    const int core = net.get_transition(petri::TransitionId{transition}).core;
    if (core < 0) {
      const int periodic_task = transition < task_of_periodic_transition_.size()
                                    ? task_of_periodic_transition_[transition]
                                    : -1;
      if (periodic_task >= 0) {
        const TaskChain& task = tasks_[static_cast<size_t>(periodic_task)];
        // A completed job is announced by an exit token. Defer the next release
        // until the zero-time consumer has removed it: this preserves timing
        // and prevents a later completion from merging into the pending token.
        if (task.exit_place < state.marking.size() && state.marking[task.exit_place] > 0) {
          continue;
        }
        // If the job is still waiting at the entry, the release's post arc is
        // clamped by the saturating capacity-1 place: the release is merged
        // into the pending job and does not reset its release clock. If the
        // job has left the entry but not completed, the post arc creates a
        // second job token in the chain; the chain-token guard below rejects
        // that overlapping-release successor instead of approximating a queue.
      }
      control.push_back(transition);
      continue;
    }
    const int ordinal =
        transition < task_of_transition_.size() ? task_of_transition_[transition] : -1;
    if (ordinal < 0) {
      throw std::invalid_argument("EDF cannot schedule real-core transition T" +
                                  std::to_string(transition) +
                                  " because it does not belong to a task chain");
    }
    enabled_per_task[static_cast<size_t>(ordinal)].push_back(transition);
  }

  // Candidate jobs: every in-flight task must have an enabled chain transition
  // (no blocking exists in the supported class). A task without an in-flight
  // job must not expose a chain transition; the only chain token it can hold is
  // a completed exit token consumed by a control transition.
  //
  // Tasks are statically partitioned: EDF orders jobs only against the other
  // in-flight jobs on their own core. Release clocks of different cores are
  // never compared.
  std::map<int, std::vector<size_t>> candidates_by_core;
  for (size_t ordinal = 0; ordinal < tasks_.size(); ++ordinal) {
    const size_t chain_tokens = task_chain_token_count(tasks_[ordinal], state.marking);
    if (chain_tokens > 1) {
      throw std::invalid_argument(
          "EDF job layout is ambiguous: task '" + tasks_[ordinal].name + "' has " +
          std::to_string(chain_tokens) +
          " tokens in its task chain; the supported class allows one job per task "
          "(a periodic release fired while the previous job was still in flight)");
    }
    const bool in_flight = task_in_flight(tasks_[ordinal], state.marking);
    const bool has_enabled = !enabled_per_task[ordinal].empty();
    if (in_flight && !has_enabled) {
      throw std::invalid_argument("EDF job layout is ambiguous: in-flight task '" +
                                  tasks_[ordinal].name +
                                  "' has no structurally enabled chain transition");
    }
    if (!in_flight && has_enabled) {
      throw std::invalid_argument(
          "EDF job layout is ambiguous: task '" + tasks_[ordinal].name +
          "' has an enabled chain transition but no in-flight job/release clock");
    }
    if (in_flight) {
      candidates_by_core[tasks_[ordinal].core].push_back(ordinal);
    }
  }

  if (candidates_by_core.empty()) {
    // Only control (environment) transitions can fire.
    std::sort(control.begin(), control.end());
    std::vector<PolicyBranch> result;
    result.push_back(PolicyBranch{std::move(control), {}});
    return result;
  }

  std::map<int, std::vector<CoreChoice>> choices_by_core;
  for (const auto& [core, candidates] : candidates_by_core) {
    std::vector<CoreChoice> choices;
    choices.reserve(candidates.size());
    for (size_t candidate : candidates) {
      const int candidate_clock = state.release_index(candidate);
      if (candidate_clock <= 0) {
        throw std::invalid_argument("EDF state is missing the release clock of in-flight task '" +
                                    task_name_or_ordinal(tasks_, candidate) + "'");
      }

      // A candidate is feasible when the zone admits a valuation in which its
      // absolute deadline is no later than every other in-flight job's *on the
      // same core*:
      //   D_i - r_i <= D_j - r_j  <=>  r_j - r_i <= D_j - D_i.
      DBM trial = state.zone;
      std::vector<BranchConstraint> constraints;
      bool feasible = true;
      for (size_t other : candidates) {
        if (other == candidate) {
          continue;
        }
        const int other_clock = state.release_index(other);
        if (other_clock <= 0) {
          throw std::invalid_argument("EDF state is missing the release clock of in-flight task '" +
                                      task_name_or_ordinal(tasks_, other) + "'");
        }
        const long long bound = static_cast<long long>(tasks_[other].deadline) -
                                static_cast<long long>(tasks_[candidate].deadline);
        if (bound < std::numeric_limits<int>::min() || bound > std::numeric_limits<int>::max()) {
          throw std::invalid_argument("EDF deadline difference does not fit in an int");
        }
        if (!trial.tighten(static_cast<size_t>(other_clock), static_cast<size_t>(candidate_clock),
                           static_cast<int>(bound))) {
          feasible = false;
          break;
        }
        constraints.push_back(BranchConstraint{candidate, other, static_cast<int>(bound)});
      }
      if (!feasible) {
        continue;
      }

      CoreChoice choice;
      choice.active = enabled_per_task[candidate];
      std::sort(choice.active.begin(), choice.active.end());
      choice.constraints = std::move(constraints);
      choices.push_back(std::move(choice));
    }

    if (choices.empty()) {
      throw std::runtime_error(
          "EDF deadline ordering is inconsistent with the state zone on core " +
          std::to_string(core) + ": no candidate can be earliest");
    }
    choices_by_core.emplace(core, std::move(choices));
  }

  // One branch per Cartesian combination of the independent per-core choices,
  // carrying the global control transitions once.
  return combine_core_choices(control, choices_by_core);
}

// ---------------------------------------------------------------------------
// Event-driven LLF
// ---------------------------------------------------------------------------

LlfFiringRule::LlfFiringRule(const petri::PTPN& net) {
  ensure_index(net);
}

void LlfFiringRule::ensure_index(const petri::PTPN& net) const {
  if (net_ == &net) {
    return;
  }

  ReleaseClassOptions options;
  options.label = "LLF";
  options.require_deadline = true;
  // LLF is a resume policy, so the lowerer installs no structural CPU place;
  // the supported core range comes from the statically partitioned TaskInfo
  // cores. Each core is scheduled independently below.
  options.use_cpu_places = false;
  options.require_deterministic_segments = true;
  ReleaseTaskIndex index = build_release_task_index(net, options);
  tasks_ = std::move(index.tasks);
  task_of_transition_ = std::move(index.task_of_transition);
  task_of_periodic_transition_ = std::move(index.task_of_periodic_transition);
  net_ = &net;
}

int LlfFiringRule::effective_priority(const PolicyContext& /*context*/,
                                      size_t /*transition*/) const {
  throw std::logic_error(
      "Event-driven LLF does not have a scalar static priority; scheduling is "
      "represented by laxity clocks and maximum-chi branches");
}

std::vector<PolicyBranch> LlfFiringRule::branches(const TransitionSet& struct_enabled,
                                                  const petri::PTPN& net,
                                                  const PolicyContext& context) const {
  ensure_index(net);
  if (context.state == nullptr) {
    throw std::invalid_argument(
        "LLF branch selection requires a prepared state class with laxity clocks");
  }
  const StateClass& state = *context.state;
  if (state.marking.size() != net.num_places()) {
    throw std::invalid_argument("LLF state marking does not match the net");
  }
  if (state.laxity_clock_of_task.size() != tasks_.size()) {
    throw std::invalid_argument("LLF state does not carry the laxity-clock layout for every task");
  }

  TransitionSet control;
  std::vector<TransitionSet> enabled_per_task(tasks_.size());
  for (size_t transition : struct_enabled) {
    const int core = net.get_transition(petri::TransitionId{transition}).core;
    if (core < 0) {
      const int periodic_task = transition < task_of_periodic_transition_.size()
                                    ? task_of_periodic_transition_[transition]
                                    : -1;
      if (periodic_task >= 0) {
        const TaskChain& task = tasks_[static_cast<size_t>(periodic_task)];
        // Defer a periodic release while a completed job's exit token is
        // pending; the zero-time consumer runs first. If the job is still
        // waiting at the capacity-1 entry, the post arc saturates and no new job
        // is created: the pending job keeps its original release age and
        // laxity clock. If the job is mid-chain, the release creates a second
        // chain token and the guard below rejects the overlapping release.
        if (task.exit_place < state.marking.size() && state.marking[task.exit_place] > 0) {
          continue;
        }
      }
      control.push_back(transition);
      continue;
    }
    const int ordinal =
        transition < task_of_transition_.size() ? task_of_transition_[transition] : -1;
    if (ordinal < 0) {
      throw std::invalid_argument("LLF cannot schedule real-core transition T" +
                                  std::to_string(transition) +
                                  " because it does not belong to a task chain");
    }
    enabled_per_task[static_cast<size_t>(ordinal)].push_back(transition);
  }

  // Candidate jobs: every in-flight task must have an enabled chain transition.
  // A task without an in-flight job must not expose a chain transition; the
  // only chain token it can hold is a completed exit token consumed by a
  // control transition.
  //
  // Tasks are statically partitioned: LLF-ED ranks jobs only against the other
  // in-flight jobs on their own core. Laxity clocks of different cores are
  // never compared.
  std::map<int, std::vector<size_t>> candidates_by_core;
  for (size_t ordinal = 0; ordinal < tasks_.size(); ++ordinal) {
    const size_t chain_tokens = task_chain_token_count(tasks_[ordinal], state.marking);
    if (chain_tokens > 1) {
      throw std::invalid_argument(
          "LLF job layout is ambiguous: task '" + tasks_[ordinal].name + "' has " +
          std::to_string(chain_tokens) +
          " tokens in its task chain; the supported class allows one job per task "
          "(a release fired while the previous job was still in flight, i.e. an "
          "overlapping release)");
    }
    const bool in_flight = task_in_flight(tasks_[ordinal], state.marking);
    const bool has_enabled = !enabled_per_task[ordinal].empty();
    if (in_flight && !has_enabled) {
      throw std::invalid_argument("LLF job layout is ambiguous: in-flight task '" +
                                  tasks_[ordinal].name +
                                  "' has no structurally enabled chain transition");
    }
    if (!in_flight && has_enabled) {
      throw std::invalid_argument(
          "LLF job layout is ambiguous: task '" + tasks_[ordinal].name +
          "' has an enabled chain transition but no in-flight job/laxity clock");
    }
    if (in_flight) {
      candidates_by_core[tasks_[ordinal].core].push_back(ordinal);
    }
  }

  std::map<int, std::vector<CoreChoice>> choices_by_core;
  for (const auto& [core, candidates] : candidates_by_core) {
    std::vector<CoreChoice> choices;
    choices.reserve(candidates.size());
    for (size_t candidate : candidates) {
      const int candidate_clock = state.laxity_index(candidate);
      if (candidate_clock <= 0) {
        throw std::invalid_argument("LLF state is missing the laxity clock of in-flight task '" +
                                    task_name_or_ordinal(tasks_, candidate) + "'");
      }

      // Candidate i is feasible when the zone admits a valuation in which its
      // urgency is at least every other in-flight job's *on the same core*:
      //   chi_i >= chi_j  <=>  chi_j - chi_i <= 0.
      // Non-strict constraints keep every equal-urgency tie feasible.
      DBM trial = state.zone;
      std::vector<BranchConstraint> constraints;
      bool feasible = true;
      for (size_t other : candidates) {
        if (other == candidate) {
          continue;
        }
        const int other_clock = state.laxity_index(other);
        if (other_clock <= 0) {
          throw std::invalid_argument("LLF state is missing the laxity clock of in-flight task '" +
                                      task_name_or_ordinal(tasks_, other) + "'");
        }
        if (!trial.tighten(static_cast<size_t>(other_clock), static_cast<size_t>(candidate_clock),
                           0)) {
          feasible = false;
          break;
        }
        constraints.push_back(BranchConstraint{candidate, other, 0, ClockKind::Laxity});
      }
      if (!feasible) {
        continue;
      }

      CoreChoice choice;
      choice.active = enabled_per_task[candidate];
      std::sort(choice.active.begin(), choice.active.end());
      choice.constraints = std::move(constraints);
      choices.push_back(std::move(choice));
    }

    if (choices.empty()) {
      throw std::runtime_error("LLF urgency ordering is inconsistent with the state zone on core " +
                               std::to_string(core) + ": no candidate can have maximum chi");
    }
    choices_by_core.emplace(core, std::move(choices));
  }

  // One branch per Cartesian combination of the independent per-core choices,
  // carrying the global control transitions once. A core with no in-flight
  // task contributes no choice.
  return combine_core_choices(control, choices_by_core);
}

// ---------------------------------------------------------------------------
// FIFO
// ---------------------------------------------------------------------------

FifoFiringRule::FifoFiringRule(const petri::PTPN& net) {
  ensure_index(net);
}

void FifoFiringRule::ensure_index(const petri::PTPN& net) const {
  if (net_ == &net) {
    return;
  }

  ReleaseClassOptions options;
  options.label = "FIFO";
  options.require_deadline = false;
  // FIFO is not a resume policy, so the lowerer must install the structural
  // core places (capacity `cores_per_cpu`, and `cores_per_cpu == 1` is
  // required) and non-suspendable execution segments. The number of CPU places
  // defines the supported core range; a hand-built net without a CPU place is
  // still accepted and derives the per-core running set from the marking.
  options.use_cpu_places = true;
  ReleaseTaskIndex index = build_release_task_index(net, options);
  tasks_ = std::move(index.tasks);
  task_of_transition_ = std::move(index.task_of_transition);
  task_of_periodic_transition_ = std::move(index.task_of_periodic_transition);
  net_ = &net;
}

int FifoFiringRule::effective_priority(const PolicyContext& /*context*/,
                                       size_t /*transition*/) const {
  throw std::logic_error(
      "FIFO does not have a scalar static priority; scheduling is represented by "
      "release clocks and arrival-order branches");
}

std::vector<PolicyBranch> FifoFiringRule::branches(const TransitionSet& struct_enabled,
                                                   const petri::PTPN& net,
                                                   const PolicyContext& context) const {
  ensure_index(net);
  if (context.state == nullptr) {
    throw std::invalid_argument(
        "FIFO branch selection requires a prepared state class with release clocks");
  }
  const StateClass& state = *context.state;
  if (state.marking.size() != net.num_places()) {
    throw std::invalid_argument("FIFO state marking does not match the net");
  }
  if (state.release_clock_of_task.size() != tasks_.size()) {
    throw std::invalid_argument(
        "FIFO state does not carry the release-clock layout for every task");
  }

  TransitionSet control;
  std::vector<TransitionSet> enabled_per_task(tasks_.size());
  for (size_t transition : struct_enabled) {
    const int core = net.get_transition(petri::TransitionId{transition}).core;
    if (core < 0) {
      const int periodic_task = transition < task_of_periodic_transition_.size()
                                    ? task_of_periodic_transition_[transition]
                                    : -1;
      if (periodic_task >= 0) {
        const TaskChain& task = tasks_[static_cast<size_t>(periodic_task)];
        // A completed job is announced by an exit token. Defer the next release
        // until the zero-time consumer has removed it: this preserves timing
        // and prevents a later completion from merging into the pending token.
        if (task.exit_place < state.marking.size() && state.marking[task.exit_place] > 0) {
          continue;
        }
        // If the job is still waiting at the entry, the release's post arc is
        // clamped by the saturating capacity-1 place: the release is merged
        // into the pending job and does not reset its release clock. If the
        // job has left the entry but not completed, the post arc creates a
        // second job token in the chain; the chain-token guard below rejects
        // that overlapping-release successor instead of approximating a queue.
      }
      control.push_back(transition);
      continue;
    }
    const int ordinal =
        transition < task_of_transition_.size() ? task_of_transition_[transition] : -1;
    if (ordinal < 0) {
      throw std::invalid_argument("FIFO cannot schedule real-core transition T" +
                                  std::to_string(transition) +
                                  " because it does not belong to a task chain");
    }
    enabled_per_task[static_cast<size_t>(ordinal)].push_back(transition);
  }

  // Classify every task from its marking: pending at the entry, running
  // somewhere else in the chain, or completed. The one-job supported class
  // never has more than one token per chain. Tasks are statically partitioned,
  // so the classification and every scheduling decision are per core.
  std::map<int, std::vector<size_t>> pending_by_core;
  std::map<int, size_t> running_by_core;
  for (size_t ordinal = 0; ordinal < tasks_.size(); ++ordinal) {
    const TaskChain& task = tasks_[ordinal];
    const size_t chain_tokens = task_chain_token_count(task, state.marking);
    if (chain_tokens > 1) {
      throw std::invalid_argument(
          "FIFO job layout is ambiguous: task '" + task.name + "' has " +
          std::to_string(chain_tokens) +
          " tokens in its task chain; the supported class allows one job per task "
          "(a periodic release fired while the previous job was still in flight)");
    }

    const bool in_flight = task_in_flight(task, state.marking);
    const bool has_enabled = !enabled_per_task[ordinal].empty();
    if (!in_flight) {
      if (has_enabled) {
        throw std::invalid_argument(
            "FIFO job layout is ambiguous: task '" + task.name +
            "' has an enabled chain transition but no in-flight job/release clock");
      }
      continue;  // completed job (exit token) or entirely idle task
    }

    const bool at_entry =
        task.entry_place < state.marking.size() && state.marking[task.entry_place] > 0;
    if (at_entry) {
      // While another job runs on this core the structural core place disables
      // this pending job's get_core transition. That is the expected
      // non-preemptive state, so an empty enabled set is checked only once the
      // core is free.
      pending_by_core[task.core].push_back(ordinal);
      continue;
    }

    // Mid-chain token: the job has been dispatched and runs to completion.
    const auto [it, inserted] = running_by_core.emplace(task.core, ordinal);
    if (!inserted) {
      throw std::invalid_argument(
          "FIFO supported class allows at most one running job per real core; tasks '" +
          tasks_[it->second].name + "' and '" + task.name +
          "' both have a job past the task entry on core " + std::to_string(task.core));
    }
  }

  // Per-core choices. A core with no in-flight job is absent and contributes
  // nothing; control transitions are added once by combine_core_choices().
  std::map<int, std::vector<CoreChoice>> choices_by_core;

  // Non-preemption: while a job runs on a core, only its own chain transitions
  // may be active on that core. A pending job on the same core cannot dispatch,
  // even in a hand-built net without a structural CPU place. Independent cores
  // still dispatch concurrently through the Cartesian product below.
  for (const auto& [core, ordinal] : running_by_core) {
    if (enabled_per_task[ordinal].empty()) {
      throw std::invalid_argument(
          "FIFO job layout is ambiguous: running task '" + tasks_[ordinal].name +
          "' has no structurally enabled chain transition on core " + std::to_string(core));
    }
    CoreChoice choice;
    choice.active = enabled_per_task[ordinal];
    std::sort(choice.active.begin(), choice.active.end());
    choices_by_core[core].push_back(std::move(choice));
  }

  // Cores with no running job are free: every pending job on such a core is a
  // dispatch candidate. Arrival order is local to the core. A release clock is
  // the time since release and always advances at rate 1, so the largest value
  // is the earliest arrival. Candidate i is feasible iff
  //   r_i >= r_j  <=>  r_j - r_i <= 0  for every other pending job j
  // on the same core, represented as BranchConstraint{i, j, 0}. Ties keep every
  // tied candidate, so independent ties on different cores produce the full
  // Cartesian product.
  for (const auto& [core, pending] : pending_by_core) {
    if (running_by_core.count(core) != 0) {
      continue;  // pending jobs wait behind the running job on this core
    }

    // The core is free: every pending job must be able to start. A pending job
    // without an enabled start cannot be dispatched and would make the FIFO
    // order unrepresentable.
    for (size_t ordinal : pending) {
      if (enabled_per_task[ordinal].empty()) {
        throw std::invalid_argument(
            "FIFO job layout is ambiguous: pending task '" + tasks_[ordinal].name +
            "' has no structurally enabled start transition while its core is free");
      }
    }

    std::vector<CoreChoice> choices;
    choices.reserve(pending.size());
    for (size_t candidate : pending) {
      const int candidate_clock = state.release_index(candidate);
      if (candidate_clock <= 0) {
        throw std::invalid_argument("FIFO state is missing the release clock of pending task '" +
                                    task_name_or_ordinal(tasks_, candidate) + "'");
      }

      DBM trial = state.zone;
      std::vector<BranchConstraint> constraints;
      bool feasible = true;
      for (size_t other : pending) {
        if (other == candidate) {
          continue;
        }
        const int other_clock = state.release_index(other);
        if (other_clock <= 0) {
          throw std::invalid_argument("FIFO state is missing the release clock of pending task '" +
                                      task_name_or_ordinal(tasks_, other) + "'");
        }
        if (!trial.tighten(static_cast<size_t>(other_clock), static_cast<size_t>(candidate_clock),
                           0)) {
          feasible = false;
          break;
        }
        constraints.push_back(BranchConstraint{candidate, other, 0});
      }
      if (!feasible) {
        continue;
      }

      CoreChoice choice;
      choice.active = enabled_per_task[candidate];
      std::sort(choice.active.begin(), choice.active.end());
      choice.constraints = std::move(constraints);
      choices.push_back(std::move(choice));
    }

    if (choices.empty()) {
      throw std::runtime_error(
          "FIFO arrival ordering is inconsistent with the state zone on core " +
          std::to_string(core) + ": no pending job can be earliest");
    }
    choices_by_core.emplace(core, std::move(choices));
  }

  // One branch per Cartesian combination of the independent per-core choices,
  // carrying the global control transitions once.
  return combine_core_choices(control, choices_by_core);
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

const std::vector<int>& PipFiringRule::effective_priorities(const petri::PTPN& net,
                                                            const petri::Marking& marking) const {
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

  const detail::TaskLockTopology::MarkingState layout = topology.analyze_marking(marking);
  std::vector<std::pair<int, int>> wait_edges;
  for (const auto& [waiter, lock] : layout.blocked_requests) {
    const auto holder_it = layout.unique_holder.find(lock);
    if (holder_it == layout.unique_holder.end() || holder_it->second == waiter) {
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

int PipFiringRule::effective_priority(const PolicyContext& context, size_t transition) const {
  ensure_index(context.net);
  const size_t num_transitions = index_->task_of_transition().size();
  if (transition < num_transitions) {
    const int task = index_->task_of_transition()[transition];
    if (task >= 0 && context.state != nullptr) {
      const std::vector<int>& effective = effective_priorities(context.net, context.state->marking);
      if (static_cast<size_t>(task) < effective.size()) {
        return effective[static_cast<size_t>(task)];
      }
    }
  }
  // Control transitions and transitions outside every task chain keep their
  // statically encoded priority.
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool PipFiringRule::admissible(const PolicyContext& /*context*/, size_t /*transition*/) const {
  // PIP only changes effective priorities; it never blocks a transition.
  return true;
}

// ---------------------------------------------------------------------------
// PCP
// ---------------------------------------------------------------------------

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

  inherit_priorities(state.effective, wait_edges, task_count);

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

// ---------------------------------------------------------------------------
// SRP
// ---------------------------------------------------------------------------

SrpFiringRule::SrpFiringRule(const petri::PTPN& net) {
  index_ = std::make_unique<detail::TaskLockTopology>();
  index_->build(net);
}

void SrpFiringRule::ensure_index(const petri::PTPN& net) const {
  if (!index_ || !index_->built_for(net)) {
    index_ = std::make_unique<detail::TaskLockTopology>();
    index_->build(net);
    cache_valid_ = false;
  }
}

std::optional<int> SrpFiringRule::lock_ceiling(const std::string& lock) const {
  if (!index_ || index_->net() == nullptr) {
    return std::nullopt;
  }
  return index_->lock_ceiling(lock);
}

const detail::TaskLockTopology::MarkingState& SrpFiringRule::dynamic_state(
    const petri::PTPN& net, const petri::Marking& marking) const {
  ensure_index(net);

  if (cache_valid_ && cached_net_ == &net && cached_marking_ == marking) {
    return cached_layout_;
  }

  cached_layout_ = index_->analyze_marking(marking);
  cached_marking_ = marking;
  cached_net_ = &net;
  cache_valid_ = true;
  return cached_layout_;
}

int SrpFiringRule::effective_priority(const PolicyContext& context, size_t transition) const {
  // SRP has no priority inheritance: every transition keeps its statically
  // encoded priority. For task chains this equals the task's base priority;
  // control and non-task transitions keep their static priority.
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool SrpFiringRule::admissible(const PolicyContext& context, size_t transition) const {
  if (context.state == nullptr) {
    return true;  // no marking: no ceiling information, no restriction
  }

  ensure_index(context.net);
  const size_t num_transitions = index_->task_of_transition().size();
  if (transition >= num_transitions) {
    return true;  // control/other transition
  }
  const int task = index_->task_of_transition()[transition];
  if (task < 0) {
    return true;  // control/other transition
  }
  const detail::TaskLockTopology::TaskRecord& record = index_->tasks()[static_cast<size_t>(task)];

  // Only the get_core transition (chain position 1) opens a task. Every other
  // transition is admissible regardless of the system ceiling.
  if (record.chain.size() < 2 || !record.chain[1].is_transition() ||
      record.chain[1].as_transition().index() != transition) {
    return true;
  }

  const petri::Marking& marking = context.state->marking;
  if (marking.size() != context.net.num_places()) {
    return true;  // defensive fallback: cannot reason about this marking
  }

  const detail::TaskLockTopology::MarkingState& layout = dynamic_state(context.net, marking);
  const size_t ordinal = static_cast<size_t>(task);
  if (ordinal >= layout.supported.size() || !layout.supported[ordinal]) {
    // Unsupported/ambiguous chain: skip the admission check. This is a
    // defensive fallback, not a certified SRP-exact state.
    return true;
  }
  if (layout.token_position[ordinal] != 0) {
    return true;  // the task already started; SRP never blocks it again
  }

  const std::optional<int> ceiling = index_->system_ceiling(layout);
  if (!ceiling) {
    return true;  // no lock held: empty system ceiling
  }
  return record.base_priority > *ceiling;
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
    case SchedulePolicy::PCP:
      // Net-aware behavior is installed lazily from PolicyContext::net.
      return std::make_unique<PcpFiringRule>();
    case SchedulePolicy::SRP:
      // Net-aware behavior is installed lazily from PolicyContext::net.
      return std::make_unique<SrpFiringRule>();
    case SchedulePolicy::FIXED_PRIOR_WITH_RESTART:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::RESTART);
    case SchedulePolicy::EDF:
      // Net-aware behavior is installed lazily from PolicyContext::net; the
      // release-clock metadata is validated on first index build.
      return std::make_unique<EdfFiringRule>();
    case SchedulePolicy::FIFO:
      // Arrival clocks and non-preemptive branches; validated on first index
      // build from PolicyContext::net.
      return std::make_unique<FifoFiringRule>();
    case SchedulePolicy::LLF:
      // Event-driven LLF: laxity clocks and maximum-chi branches; validated on
      // first index build from PolicyContext::net. Never falls back to static
      // priorities.
      return std::make_unique<LlfFiringRule>();
    case SchedulePolicy::UNKNOWN:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::NON_PREEMPTIVE);
  }
  return std::make_unique<StaticPriorityRule>(PreemptionMode::NON_PREEMPTIVE);
}

std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy, const petri::PTPN& net) {
  if (policy == SchedulePolicy::EDF) {
    return std::make_unique<EdfFiringRule>(net);
  }
  if (policy == SchedulePolicy::FIFO) {
    return std::make_unique<FifoFiringRule>(net);
  }
  if (policy == SchedulePolicy::LLF) {
    return std::make_unique<LlfFiringRule>(net);
  }
  if (policy == SchedulePolicy::PIP) {
    return std::make_unique<PipFiringRule>(net);
  }
  if (policy == SchedulePolicy::PCP) {
    return std::make_unique<PcpFiringRule>(net);
  }
  if (policy == SchedulePolicy::SRP) {
    return std::make_unique<SrpFiringRule>(net);
  }
  return make_firing_rule(policy);
}

}  // namespace state_class
