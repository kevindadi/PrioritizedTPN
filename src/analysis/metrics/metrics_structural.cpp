#include <algorithm>

#include "analysis/dbm/clock_state.h"
#include "analysis/metrics/metrics.h"
#include "analysis/metrics/metrics_internal.h"

namespace state_class {

void MetricsAnalyzer::compute_structural(MetricsReport& report) {
  report.max_tokens_per_place.assign(net_.num_places(), 0);
  for (size_t v = 0; v < num_vertices_; ++v) {
    const std::vector<int>& m = state_of_[v]->marking;
    for (size_t p = 0; p < m.size() && p < report.max_tokens_per_place.size(); ++p) {
      report.max_tokens_per_place[p] = std::max(report.max_tokens_per_place[p], m[p]);
    }
  }
  report.bounded = true;
  for (size_t p : petri::overflowed_places()) {
    if (p < net_.num_places()) {
      report.bounded = false;
      report.overflow_places.push_back(net_.get_place(petri::PlaceId{p}).name);
    }
  }

  // Illegitimate sinks: no successor while a task chain still holds a token.
  // Under horizon truncation a boundary class has no out-edges by construction,
  // so sink detection would report false deadlocks. A max-states cut has the
  // same artifact: successors dropped at the cap leave the source without
  // out-edges. Both cases disable sink detection instead of reporting a false
  // counterexample.
  if (!report.deadlock_analysis_certified || graph_truncated_) {
    return;
  }
  for (size_t v = 0; v < num_vertices_; ++v) {
    if (!out_edges_[v].empty()) {
      continue;
    }
    bool work_remaining = false;
    for (const TaskTopology& task : tasks_) {
      if (task_in_flight(task, v)) {
        work_remaining = true;
        break;
      }
    }
    if (work_remaining) {
      report.deadlock_states.push_back(v);
    }
  }
}

void MetricsAnalyzer::compute_schedulability(MetricsReport& report) {
  // Schedulable requires every observed deadline to be met on a complete
  // analysis; unschedulable requires a proven counterexample (a certified
  // miss, starvation, or a reachable deadlock). Everything else is unknown.
  bool unschedulable = !report.deadlock_states.empty();
  bool unknown = !report.deadlock_analysis_certified;
  for (const auto& task : report.tasks) {
    if (task.local_deadlock) {
      unschedulable = true;
      continue;
    }
    if (!task.has_deadline) {
      continue;
    }
    switch (task.verdict) {
      case DeadlineVerdict::Misses:
        unschedulable = true;
        break;
      case DeadlineVerdict::Unknown:
        unknown = true;
        break;
      case DeadlineVerdict::Meets:
        break;
    }
  }
  report.schedulability = unschedulable ? Schedulability::Unschedulable
                                        : (unknown ? Schedulability::Unknown
                                                   : Schedulability::Schedulable);
  report.schedulable = report.schedulability == Schedulability::Schedulable;
}

}  // namespace state_class
