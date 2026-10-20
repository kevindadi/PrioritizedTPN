#include "analysis/reachability/scheduling/fifo.h"

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

FifoFiringRule::FifoFiringRule(const petri::PTPN& net) {
  ensure_index(net);
}

void FifoFiringRule::ensure_index(const petri::PTPN& net) const {
  if (net_ == &net) {
    return;
  }

  detail::ReleaseClassOptions options;
  options.label = "FIFO";
  options.require_deadline = false;
  // FIFO is not a resume policy, so the lowerer must install the structural
  // core places (capacity `cores_per_cpu`, and `cores_per_cpu == 1` is
  // required) and non-suspendable execution segments. The number of CPU places
  // defines the supported core range; a hand-built net without a CPU place is
  // still accepted and derives the per-core running set from the marking.
  options.use_cpu_places = true;
  detail::ReleaseTaskIndex index = detail::build_release_task_index(net, options);
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
      throw ptpn::UnsupportedClassError(
          "FIFO job layout is ambiguous: task '" + task.name + "' has " +
              std::to_string(chain_tokens) +
              " tokens in its task chain; the supported class allows one job per task "
              "(a periodic release fired while the previous job was still in flight)",
          "task:" + task.name);
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
      throw ptpn::UnsupportedClassError(
          "FIFO supported class allows at most one running job per real core; tasks '" +
              tasks_[it->second].name + "' and '" + task.name +
              "' both have a job past the task entry on core " + std::to_string(task.core),
          "task:" + task.name);
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
                                    detail::task_name_or_ordinal(tasks_, candidate) + "'");
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
                                      detail::task_name_or_ordinal(tasks_, other) + "'");
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

}  // namespace state_class
