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
  // so sink detection would report false deadlocks; it is disabled instead.
  if (!report.deadlock_analysis_certified) {
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
  // The paper's definition is WCRT(a) <= DL(a) for every task, together with
  // the absence of reachable deadlocks. `deadline_missed` uses the sound WCRT
  // upper bound, so a schedulable verdict is conservative.
  report.schedulable = report.deadlock_states.empty();
  for (const auto& task : report.tasks) {
    if (task.has_deadline && task.deadline_missed) {
      report.schedulable = false;
    }
    if (task.local_deadlock) {
      report.schedulable = false;
    }
  }
}

}  // namespace state_class
