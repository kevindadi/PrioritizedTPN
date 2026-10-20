#include "analysis/reachability/scheduling/llf.h"

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

LlfFiringRule::LlfFiringRule(const petri::PTPN& net) {
  ensure_index(net);
}

void LlfFiringRule::ensure_index(const petri::PTPN& net) const {
  if (net_ == &net) {
    return;
  }

  detail::ReleaseClassOptions options;
  options.label = "LLF";
  options.require_deadline = true;
  // LLF is a resume policy, so the lowerer installs no structural CPU place;
  // the supported core range comes from the statically partitioned TaskInfo
  // cores. Each core is scheduled independently below.
  options.use_cpu_places = false;
  options.require_deterministic_segments = true;
  detail::ReleaseTaskIndex index = detail::build_release_task_index(net, options);
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
      throw ptpn::UnsupportedClassError(
          "LLF job layout is ambiguous: task '" + tasks_[ordinal].name + "' has " +
              std::to_string(chain_tokens) +
              " tokens in its task chain; the supported class allows one job per task "
              "(a release fired while the previous job was still in flight, i.e. an "
              "overlapping release)",
          "task:" + tasks_[ordinal].name);
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
                                    detail::task_name_or_ordinal(tasks_, candidate) + "'");
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
                                      detail::task_name_or_ordinal(tasks_, other) + "'");
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

}  // namespace state_class
