#include "analysis/reachability/scheduling/edf.h"

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
      throw ptpn::UnsupportedClassError(
          "EDF job layout is ambiguous: task '" + tasks_[ordinal].name + "' has " +
              std::to_string(chain_tokens) +
              " tokens in its task chain; the supported class allows one job per task "
              "(a periodic release fired while the previous job was still in flight)",
          "task:" + tasks_[ordinal].name);
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

}  // namespace state_class
