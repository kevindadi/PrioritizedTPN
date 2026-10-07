#include <algorithm>
#include <functional>

#include "analysis/clock_state.h"
#include "analysis/metrics.h"
#include "analysis/metrics_internal.h"

namespace state_class {

namespace {

// Accumulator used by the longest/shortest path DP.
struct DPResult {
  bool reachable = false;
  bool infinite = false;
  long long value = 0;
};

DPResult combine_max(const DPResult& a, const DPResult& b) {
  if (!a.reachable)
    return b;
  if (!b.reachable)
    return a;
  DPResult r;
  r.reachable = true;
  if (a.infinite || b.infinite) {
    r.infinite = true;
  } else {
    r.value = std::max(a.value, b.value);
  }
  return r;
}

DPResult combine_min(const DPResult& a, const DPResult& b) {
  if (!a.reachable)
    return b;
  if (!b.reachable)
    return a;
  DPResult r;
  r.reachable = true;
  if (a.infinite && b.infinite) {
    r.infinite = true;
  } else if (a.infinite) {
    r = b;
  } else if (b.infinite) {
    r = a;
  } else {
    r.value = std::min(a.value, b.value);
  }
  return r;
}

TimeValue to_time(const DPResult& r) {
  TimeValue t;
  t.infinite = r.infinite;
  t.value = r.value;
  return t;
}

}  // namespace

void MetricsAnalyzer::compute_task_timing(MetricsReport& report) {
  // Generic DP: accumulate `weight` along in-flight paths of `task` up to a
  // completion edge (end-place token increase). `maximize` controls longest vs
  // shortest; a cycle makes the longest path infinite and is skipped for the
  // shortest.
  auto run_dp = [&](const TaskTopology& task, const std::function<int(size_t, const Edge&)>& weight,
                    bool maximize, const std::vector<size_t>& starts) {
    std::vector<DPResult> memo(num_vertices_);
    std::vector<char> color(num_vertices_, 0);  // 0 white, 1 gray, 2 black

    std::function<DPResult(size_t)> dfs = [&](size_t v) -> DPResult {
      if (color[v] == 2) {
        return memo[v];
      }
      if (color[v] == 1) {
        DPResult r;
        if (maximize) {
          r.reachable = true;
          r.infinite = true;  // cycle => unbounded
        }
        return r;  // shortest path ignores cycles
      }
      color[v] = 1;
      DPResult best;
      for (const Edge& e : out_edges_[v]) {
        const int w = weight(v, e);
        const bool completes = task.has_end && state_of_[e.target]->marking[task.end_place] >
                                                   state_of_[v]->marking[task.end_place];
        if (completes) {
          DPResult cand;
          cand.reachable = true;
          if (is_inf(w)) {
            cand.infinite = true;
          } else {
            cand.value = w;
          }
          best = maximize ? combine_max(best, cand) : combine_min(best, cand);
        } else if (task_in_flight(task, e.target)) {
          const DPResult sub = dfs(e.target);
          if (!sub.reachable) {
            continue;
          }
          DPResult cand;
          cand.reachable = true;
          if (is_inf(w) || sub.infinite) {
            cand.infinite = true;
          } else {
            cand.value = static_cast<long long>(w) + sub.value;
          }
          best = maximize ? combine_max(best, cand) : combine_min(best, cand);
        }
      }
      color[v] = 2;
      memo[v] = best;
      return best;
    };

    DPResult overall;
    for (size_t s : starts) {
      const DPResult r = dfs(s);
      if (!r.reachable) {
        continue;
      }
      overall = maximize ? combine_max(overall, r) : combine_min(overall, r);
    }
    return overall;
  };

  for (const TaskTopology& task : tasks_) {
    TaskMetrics tm;
    tm.name = task.name;
    tm.core = task.core;
    tm.priority = task.priority;
    const auto info_it = net_.task_info().find(task.name);
    if (info_it != net_.task_info().end()) {
      tm.wcet = info_it->second.wcet;
      tm.bcet = info_it->second.bcet;
      tm.period = info_it->second.period;
      tm.deadline = info_it->second.deadline;
      tm.has_deadline = info_it->second.deadline > 0;
    }

    // Release states: edges that deposit a token into the entry place, plus the
    // initial state if the task starts already released.
    std::vector<size_t> release_states;
    {
      std::set<size_t> uniq;
      if (state_of_[initial_]->marking[task.entry_place] > 0) {
        uniq.insert(initial_);
        tm.activations++;  // released at t=0 (start task)
      }
      for (size_t v = 0; v < num_vertices_; ++v) {
        for (const Edge& e : out_edges_[v]) {
          if (state_of_[e.target]->marking[task.entry_place] >
              state_of_[v]->marking[task.entry_place]) {
            uniq.insert(e.target);
            tm.activations++;
          }
        }
      }
      release_states.assign(uniq.begin(), uniq.end());
    }
    tm.observed = !release_states.empty();

    // Max in-flight tokens for this task.
    for (size_t v = 0; v < num_vertices_; ++v) {
      int sum = 0;
      for (size_t p : task.chain_places) {
        sum += state_of_[v]->marking[p];
      }
      tm.max_in_flight = std::max(tm.max_in_flight, sum);
    }

    if (tm.observed) {
      const auto w_dwell_max = [&](size_t, const Edge& e) { return e.dwell_max; };
      const auto w_dwell_min = [&](size_t, const Edge& e) { return e.dwell_min; };
      const auto w_interf = [&](size_t v, const Edge& e) {
        return task_suspended(task, v) ? e.dwell_max : 0;
      };
      const auto w_block = [&](size_t v, const Edge& e) {
        return task_blocked_by_lower(task, v) ? e.dwell_max : 0;
      };
      const auto w_preempt = [&](size_t v, const Edge& e) {
        return (task_active(task, v) && task_suspended(task, e.target)) ? 1 : 0;
      };

      tm.wcrt = to_time(run_dp(task, w_dwell_max, true, release_states));
      tm.bcrt = to_time(run_dp(task, w_dwell_min, false, release_states));
      // The per-edge dwell_min sum ignores cross-state DBM correlation and can
      // dip below the execution demand. BCET is an independent, sound lower
      // bound, so lift BCRT to whichever is larger (still <= true BCRT).
      if (!tm.bcrt.infinite && tm.bcet > tm.bcrt.value) {
        tm.bcrt.value = tm.bcet;
      }
      tm.jitter.infinite = tm.wcrt.infinite;
      if (!tm.wcrt.infinite && !tm.bcrt.infinite) {
        tm.jitter.value = std::max<long long>(0, tm.wcrt.value - tm.bcrt.value);
      }
      tm.worst_interference = to_time(run_dp(task, w_interf, true, release_states));
      tm.worst_blocking = to_time(run_dp(task, w_block, true, release_states));
      const DPResult preempt = run_dp(task, w_preempt, true, release_states);
      tm.max_preemptions = preempt.infinite ? -1 : static_cast<int>(preempt.value);

      if (tm.has_deadline) {
        tm.slack.infinite = tm.wcrt.infinite;
        if (!tm.wcrt.infinite) {
          tm.slack.value = static_cast<long long>(tm.deadline) - tm.wcrt.value;
        }
      }
    }

    report.tasks.push_back(std::move(tm));
  }
}

}  // namespace state_class
