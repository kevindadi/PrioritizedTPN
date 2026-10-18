#include "analysis/reachability/scheduling.h"

#include <algorithm>
#include <map>
#include <vector>

namespace state_class {

namespace {

// Enumerates all size-k subsets of `group` starting at `start`, appending each
// (as a sorted vector) to `out`. Transition ids in `group` arrive sorted, so
// every combination is sorted by construction.
void enumerate_combinations(const std::vector<size_t>& group, size_t k, size_t start,
                            std::vector<size_t>& current, std::vector<TransitionSet>& out) {
  if (current.size() == k) {
    out.push_back(current);
    return;
  }
  const size_t remaining = k - current.size();
  for (size_t i = start; i + remaining <= group.size(); ++i) {
    current.push_back(group[i]);
    enumerate_combinations(group, k, i + 1, current, out);
    current.pop_back();
  }
}

}  // namespace

TransitionSet Scheduling::structural_enabled(const petri::PTPN& net,
                                             const petri::Marking& marking) {
  TransitionSet enabled;
  const size_t num_transitions = net.num_transitions();
  for (size_t t = 0; t < num_transitions; ++t) {
    if (petri::PTPN::is_enabled(marking, net, petri::TransitionId{t})) {
      enabled.push_back(t);
    }
  }
  return enabled;
}

std::vector<TransitionSet> Scheduling::priority_branches(const TransitionSet& struct_enabled,
                                                         const petri::PTPN& net) {
  // Every transition competes within its core group, identified by the `core`
  // attribute. The control core (-1) is not CPU-scheduled: all of its
  // maximal-priority transitions form one choice and remain concurrently
  // enabled (ordinary TPN semantics for control).
  std::map<int, std::vector<size_t>> per_core;
  for (size_t t : struct_enabled) {
    per_core[net.get_transition(petri::TransitionId{t}).core].push_back(t);
  }

  // Cartesian product of the per-core choices.
  std::vector<TransitionSet> branches(1);
  for (auto& [core, group] : per_core) {
    // Highest priority first, ties by transition index only to make the
    // enumeration order deterministic.
    std::vector<size_t> sorted = group;
    std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
      const int pa = net.get_transition(petri::TransitionId{a}).priority;
      const int pb = net.get_transition(petri::TransitionId{b}).priority;
      if (pa != pb) {
        return pa > pb;
      }
      return a < b;
    });

    std::vector<TransitionSet> choices;
    const int capacity = net.parallelism_of_core(core);
    if (capacity <= 0) {
      // Control core: keep every transition at the group's maximal priority.
      const int max_priority =
          net.get_transition(petri::TransitionId{sorted.front()}).priority;
      TransitionSet top;
      for (size_t t : sorted) {
        if (net.get_transition(petri::TransitionId{t}).priority == max_priority) {
          top.push_back(t);
        }
      }
      choices.push_back(std::move(top));
    } else if (sorted.size() <= static_cast<size_t>(capacity)) {
      // Enough slots for every structurally enabled transition.
      choices.push_back(sorted);
    } else {
      // Fixed-priority dispatch with K slots: the transitions strictly above
      // the cutoff priority are mandatory; the remaining slots are filled by
      // every equal-priority tie at the cutoff. For K = 1 this is exactly the
      // set of maximal-priority candidates.
      const int cutoff =
          net.get_transition(petri::TransitionId{sorted[static_cast<size_t>(capacity) - 1]})
              .priority;
      TransitionSet mandatory;
      std::vector<size_t> tied;
      for (size_t t : sorted) {
        const int priority = net.get_transition(petri::TransitionId{t}).priority;
        if (priority > cutoff) {
          mandatory.push_back(t);
        } else if (priority == cutoff) {
          tied.push_back(t);
        }
      }
      const size_t need = static_cast<size_t>(capacity) - mandatory.size();
      std::vector<TransitionSet> subsets;
      std::vector<size_t> current;
      enumerate_combinations(tied, need, 0, current, subsets);
      for (TransitionSet& subset : subsets) {
        TransitionSet choice = mandatory;
        choice.insert(choice.end(), subset.begin(), subset.end());
        std::sort(choice.begin(), choice.end());
        choices.push_back(std::move(choice));
      }
    }

    std::vector<TransitionSet> next;
    next.reserve(branches.size() * choices.size());
    for (const TransitionSet& prefix : branches) {
      for (const TransitionSet& choice : choices) {
        TransitionSet merged = prefix;
        merged.insert(merged.end(), choice.begin(), choice.end());
        std::sort(merged.begin(), merged.end());
        next.push_back(std::move(merged));
      }
    }
    branches = std::move(next);
  }

  std::sort(branches.begin(), branches.end());
  branches.erase(std::unique(branches.begin(), branches.end()), branches.end());
  return branches;
}

}  // namespace state_class
