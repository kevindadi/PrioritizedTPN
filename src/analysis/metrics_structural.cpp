#include <algorithm>

#include "analysis/clock_state.h"
#include "analysis/metrics.h"
#include "analysis/metrics_internal.h"

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
  report.schedulable = report.deadlock_states.empty();
}

}  // namespace state_class
