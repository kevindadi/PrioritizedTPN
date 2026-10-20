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
                                                         const petri::PTPN& net,
                                                         const FiringRule& rule,
                                                         const PolicyContext& context) {
  // First drop transitions the policy declares inadmissible. Every remaining
  // transition competes within its core group, identified by the `core`
  // attribute. The control core (-1) is not CPU-scheduled: all of its
  // maximal-priority transitions form one choice and remain concurrently
  // enabled (ordinary TPN semantics for control).
  std::map<int, std::vector<size_t>> per_core;
  for (size_t t : struct_enabled) {
    if (!rule.admissible(context, t)) {
      continue;
    }
    per_core[net.get_transition(petri::TransitionId{t}).core].push_back(t);
  }

  // Cartesian product of the per-core choices.
  std::vector<TransitionSet> branches(1);
  for (auto& [core, group] : per_core) {
    // Highest effective priority first, ties by transition id only to make the
    // enumeration order deterministic. Ties are still all enumerated below;
    // the id ordering never selects a winner.
    std::vector<size_t> sorted = group;
    std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
      const int pa = rule.effective_priority(context, a);
      const int pb = rule.effective_priority(context, b);
      if (pa != pb) {
        return pa > pb;
      }
      return a < b;
    });

    std::vector<TransitionSet> choices;
    const int capacity = net.parallelism_of_core(core);
    if (capacity <= 0) {
      // Control core: keep every transition at the group's maximal priority.
      const int max_priority = rule.effective_priority(context, sorted.front());
      TransitionSet top;
      for (size_t t : sorted) {
        if (rule.effective_priority(context, t) == max_priority) {
          top.push_back(t);
        }
      }
      choices.push_back(std::move(top));
    } else {
      // FIFO tie rule: a running transition at the group's maximal priority
      // keeps its slot and is not preempted by an equal-priority peer. Running
      // transitions below the maximal priority are preempted as usual.
      TransitionSet keep;
      if (context.running != nullptr && rule.tie_mode() == TieMode::FIFO) {
        const int max_priority = rule.effective_priority(context, sorted.front());
        for (size_t t : sorted) {
          if (contains(*context.running, t) &&
              rule.effective_priority(context, t) == max_priority) {
            keep.push_back(t);
          }
        }
      }

      std::vector<size_t> candidates;
      candidates.reserve(sorted.size() - keep.size());
      for (size_t t : sorted) {
        if (!contains(keep, t)) {
          candidates.push_back(t);
        }
      }

      const size_t free_slots =
          keep.size() >= static_cast<size_t>(capacity)
              ? 0
              : static_cast<size_t>(capacity) - keep.size();
      std::vector<TransitionSet> fills;
      if (free_slots == 0) {
        fills.emplace_back();
      } else if (candidates.size() <= free_slots) {
        // Enough slots for every remaining admissible transition.
        fills.push_back(candidates);
      } else {
        // Fixed-priority dispatch with K slots: the transitions strictly above
        // the cutoff priority are mandatory; the remaining slots are filled by
        // every equal-priority tie at the cutoff. For K = 1 this is exactly the
        // set of maximal-priority candidates.
        const int cutoff = rule.effective_priority(context, candidates[free_slots - 1]);
        TransitionSet mandatory;
        std::vector<size_t> tied;
        for (size_t t : candidates) {
          const int priority = rule.effective_priority(context, t);
          if (priority > cutoff) {
            mandatory.push_back(t);
          } else if (priority == cutoff) {
            tied.push_back(t);
          }
        }
        const size_t need = free_slots - mandatory.size();
        std::vector<size_t> current;
        std::vector<TransitionSet> tie_choices;
        if (rule.tie_mode() == TieMode::FIFO) {
          // Admission preference: at the cutoff priority, jobs already in
          // service take the remaining slots before jobs waiting for their
          // first dispatch. Both lists keep the deterministic id order.
          std::vector<size_t> admitted_cutoff;
          std::vector<size_t> other_cutoff;
          for (size_t t : tied) {
            const std::optional<bool> admitted = rule.job_admitted(context, t);
            if (admitted.has_value() && *admitted) {
              admitted_cutoff.push_back(t);
            } else {
              other_cutoff.push_back(t);
            }
          }
          if (admitted_cutoff.size() >= need) {
            enumerate_combinations(admitted_cutoff, need, 0, current, tie_choices);
          } else {
            // Every admitted candidate is selected; the remaining slots go to
            // the other candidates, enumerating the ties among them.
            const size_t rest = need - admitted_cutoff.size();
            std::vector<TransitionSet> extra;
            enumerate_combinations(other_cutoff, rest, 0, current, extra);
            for (TransitionSet& subset : extra) {
              TransitionSet choice = admitted_cutoff;
              choice.insert(choice.end(), subset.begin(), subset.end());
              std::sort(choice.begin(), choice.end());
              tie_choices.push_back(std::move(choice));
            }
          }
        } else {
          enumerate_combinations(tied, need, 0, current, tie_choices);
        }
        for (TransitionSet& subset : tie_choices) {
          TransitionSet fill = mandatory;
          fill.insert(fill.end(), subset.begin(), subset.end());
          std::sort(fill.begin(), fill.end());
          fills.push_back(std::move(fill));
        }
      }

      for (TransitionSet& fill : fills) {
        TransitionSet choice = keep;
        choice.insert(choice.end(), fill.begin(), fill.end());
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
