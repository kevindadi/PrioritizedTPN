#include "analysis/reachability/scheduling.h"

#include <algorithm>
#include <map>
#include <vector>

namespace state_class {

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

TransitionSet Scheduling::filter_priority_per_core(const TransitionSet& struct_enabled,
                                                   const petri::PTPN& net) {
  // Every transition competes within its core group, identified by the `core`
  // attribute. The control core (-1) is treated like any other group, so
  // control transitions are filtered by priority too (e.g. a resume transition
  // with a higher priority suppresses ordinary control steps for that instant).
  std::map<int, std::vector<size_t>> per_core;
  for (size_t t : struct_enabled) {
    per_core[net.get_transition(petri::TransitionId{t}).core].push_back(t);
  }

  TransitionSet active;
  for (auto& [core, group] : per_core) {
    const int capacity = net.parallelism_of_core(core);

    if (capacity <= 0) {
      // Unbounded group (control core, or nets without a parallelism model):
      // keep every transition sharing the highest priority on this core.
      int max_priority = net.get_transition(petri::TransitionId{group.front()}).priority;
      for (size_t t : group) {
        max_priority = std::max(max_priority, net.get_transition(petri::TransitionId{t}).priority);
      }
      for (size_t t : group) {
        if (net.get_transition(petri::TransitionId{t}).priority == max_priority) {
          active.push_back(t);
        }
      }
      continue;
    }

    // Bounded resource: at most `capacity` transitions may run at once, so keep
    // the highest-priority ones. Ties are broken by transition index, giving a
    // deterministic selection that enforces mutual exclusion (capacity 1) or a
    // fixed degree of parallelism (capacity = cores_per_cpu).
    std::sort(group.begin(), group.end(), [&](size_t a, size_t b) {
      const int pa = net.get_transition(petri::TransitionId{a}).priority;
      const int pb = net.get_transition(petri::TransitionId{b}).priority;
      if (pa != pb) {
        return pa > pb;
      }
      return a < b;
    });
    const size_t keep = std::min(static_cast<size_t>(capacity), group.size());
    for (size_t i = 0; i < keep; ++i) {
      active.push_back(group[i]);
    }
  }

  std::sort(active.begin(), active.end());
  return active;
}

}  // namespace state_class
