#include <algorithm>

#include "analysis/dbm/clock_state.h"
#include "analysis/metrics/metrics.h"
#include "analysis/metrics/metrics_internal.h"

namespace state_class {

void MetricsAnalyzer::compute_locks(MetricsReport& report) {
  // Map lock name -> resource place index.
  for (const auto& [lock_name, lock_place] : net_.lock_places()) {
    LockMetrics lm;
    lm.name = lock_name;
    long long total_hold = 0;
    long long total_wait = 0;
    bool hold_inf = false;
    bool wait_inf = false;

    for (size_t v = 0; v < num_vertices_; ++v) {
      if (lock_place.index() >= state_of_[v]->marking.size()) {
        continue;
      }
      const bool held = state_of_[v]->marking[lock_place.index()] == 0;
      if (!held) {
        continue;
      }
      // Longest single dwell while held, and graph-wide hold/wait sums.
      int state_dwell = 0;
      for (const Edge& e : out_edges_[v]) {
        if (is_inf(e.dwell_max)) {
          state_dwell = INF_TIME;
          break;
        }
        state_dwell = std::max(state_dwell, e.dwell_max);
      }
      if (is_inf(state_dwell)) {
        lm.worst_hold.infinite = true;
        hold_inf = true;
      } else {
        lm.worst_hold.value = std::max(lm.worst_hold.value, static_cast<long long>(state_dwell));
        total_hold += state_dwell;
      }

      // Waiting: another task that needs this lock is in flight while it is
      // held.
      bool contended = false;
      for (const TaskTopology& task : tasks_) {
        const auto info_it = net_.task_info().find(task.name);
        if (info_it == net_.task_info().end()) {
          continue;
        }
        const auto& locks = info_it->second.locks;
        if (std::find(locks.begin(), locks.end(), lock_name) == locks.end()) {
          continue;
        }
        if (task_in_flight(task, v) && !task_active(task, v)) {
          contended = true;
          break;
        }
      }
      if (contended && !is_inf(state_dwell)) {
        total_wait += state_dwell;
      } else if (contended) {
        wait_inf = true;
      }
    }

    lm.total_hold.infinite = hold_inf;
    lm.total_hold.value = total_hold;
    lm.total_wait.infinite = wait_inf;
    lm.total_wait.value = total_wait;
    report.locks.push_back(std::move(lm));
  }
  std::sort(report.locks.begin(), report.locks.end(),
            [](const LockMetrics& a, const LockMetrics& b) { return a.name < b.name; });
}

}  // namespace state_class
