#include "analysis/reachability/scheduling/policy_common.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace state_class {
namespace detail {

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

}  // namespace detail
}  // namespace state_class
