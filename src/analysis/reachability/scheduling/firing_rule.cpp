#include "analysis/reachability/scheduling/firing_rule.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "analysis/reachability/scheduling.h"

namespace state_class {

bool FiringRule::admissible(const PolicyContext& /*context*/, size_t /*transition*/) const {
  return true;
}

void FiringRule::ensure_admission_index(const petri::PTPN& net) const {
  if (admission_net_ == &net) {
    return;
  }
  admission_task_.assign(net.num_transitions(), -1);
  admission_entry_.clear();
  admission_places_.clear();

  const std::vector<TaskChain> tasks = build_task_chains(net);
  admission_entry_.reserve(tasks.size());
  admission_places_.reserve(tasks.size());
  for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
    admission_entry_.push_back(tasks[ordinal].entry_place);
    admission_places_.push_back(tasks[ordinal].places);
    for (size_t transition : tasks[ordinal].transitions) {
      if (transition < admission_task_.size()) {
        admission_task_[transition] = static_cast<int>(ordinal);
      }
    }
  }
  admission_net_ = &net;
}

std::optional<bool> FiringRule::job_admitted(const PolicyContext& context,
                                             size_t transition) const {
  if (context.state == nullptr) {
    return std::nullopt;
  }
  ensure_admission_index(context.net);
  if (transition >= admission_task_.size()) {
    return std::nullopt;
  }
  const int ordinal = admission_task_[transition];
  if (ordinal < 0) {
    return std::nullopt;
  }
  const size_t task = static_cast<size_t>(ordinal);
  const std::vector<int>& marking = context.state->marking;
  const size_t entry = admission_entry_[task];
  if (entry >= marking.size()) {
    return std::nullopt;
  }
  // Admitted: a chain token already sits past the entry place, so the job has
  // left the dispatch queue.
  for (size_t place : admission_places_[task]) {
    if (place == entry || place >= marking.size()) {
      continue;
    }
    if (marking[place] > 0) {
      return true;
    }
  }
  return false;
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

}  // namespace state_class
