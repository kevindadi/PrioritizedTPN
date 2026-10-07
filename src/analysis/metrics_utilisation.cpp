#include <algorithm>
#include <numeric>

#include "analysis/clock_state.h"
#include "analysis/metrics.h"
#include "analysis/metrics_internal.h"

namespace state_class {

namespace {

long long lcm_ll(long long a, long long b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  const long long g = std::gcd(a, b);
  return a / g * b;
}

}  // namespace

void MetricsAnalyzer::compute_utilisation(MetricsReport& report) {
  // Hyperperiod = lcm of task periods.
  long long hyper = 0;
  for (const auto& [name, info] : net_.task_info()) {
    if (info.period > 0) {
      hyper = (hyper == 0) ? info.period : lcm_ll(hyper, info.period);
    }
  }
  report.hyperperiod = hyper;
  for (auto& tm : report.tasks) {
    if (tm.period > 0 && hyper > 0) {
      tm.jobs_per_hyperperiod = static_cast<int>(hyper / tm.period);
    }
  }

  // Tarjan SCC to find the recurrent steady region reachable from initial.
  std::vector<long long> index(num_vertices_, -1);
  std::vector<long long> lowlink(num_vertices_, 0);
  std::vector<char> on_stack(num_vertices_, 0);
  std::vector<long long> scc_id(num_vertices_, -1);
  std::vector<size_t> stack;
  long long next_index = 0;
  long long scc_count = 0;

  std::function<void(size_t)> strongconnect = [&](size_t v) {
    index[v] = next_index;
    lowlink[v] = next_index;
    ++next_index;
    stack.push_back(v);
    on_stack[v] = 1;
    for (const Edge& e : out_edges_[v]) {
      if (index[e.target] < 0) {
        strongconnect(e.target);
        lowlink[v] = std::min(lowlink[v], lowlink[e.target]);
      } else if (on_stack[e.target]) {
        lowlink[v] = std::min(lowlink[v], index[e.target]);
      }
    }
    if (lowlink[v] == index[v]) {
      while (true) {
        const size_t w = stack.back();
        stack.pop_back();
        on_stack[w] = 0;
        scc_id[w] = scc_count;
        if (w == v) {
          break;
        }
      }
      ++scc_count;
    }
  };
  for (size_t v = 0; v < num_vertices_; ++v) {
    if (index[v] < 0) {
      strongconnect(v);
    }
  }

  // A recurrent SCC has a cycle (size > 1, or a self-loop). Pick the largest.
  std::vector<size_t> scc_size(static_cast<size_t>(scc_count), 0);
  for (size_t v = 0; v < num_vertices_; ++v) {
    scc_size[static_cast<size_t>(scc_id[v])]++;
  }
  std::vector<char> has_self(static_cast<size_t>(scc_count), 0);
  for (size_t v = 0; v < num_vertices_; ++v) {
    for (const Edge& e : out_edges_[v]) {
      if (scc_id[e.target] == scc_id[v]) {
        has_self[static_cast<size_t>(scc_id[v])] = 1;
      }
    }
  }
  long long best_scc = -1;
  for (long long c = 0; c < scc_count; ++c) {
    const bool recurrent = scc_size[c] > 1 || has_self[c];
    if (!recurrent) {
      continue;
    }
    if (best_scc < 0 || scc_size[c] > scc_size[best_scc]) {
      best_scc = c;
    }
  }
  report.has_steady_cycle = best_scc >= 0;
  report.recurrent_scc_size = best_scc >= 0 ? scc_size[static_cast<size_t>(best_scc)] : 0;

  // Vertices used for the graph-averaged busy fraction.
  std::vector<size_t> region;
  for (size_t v = 0; v < num_vertices_; ++v) {
    if (best_scc < 0 || scc_id[v] == best_scc) {
      region.push_back(v);
    }
  }

  // Collect all real cores.
  std::set<int> cores;
  for (const auto& [name, info] : net_.task_info()) {
    if (info.core >= 0) {
      cores.insert(info.core);
    }
  }

  for (int core : cores) {
    CoreMetrics cm;
    cm.core = core;
    // Analytic interval U = sum C_i / T_i over tasks on this core.
    double umin = 0.0;
    double umax = 0.0;
    for (const auto& [name, info] : net_.task_info()) {
      if (info.core != core || info.period <= 0) {
        continue;
      }
      umin += static_cast<double>(info.bcet) / info.period;
      umax += static_cast<double>(info.wcet) / info.period;
    }
    cm.util_min = umin;
    cm.util_max = umax;

    // Graph-averaged busy fraction over the recurrent region (approximate).
    double busy = 0.0;
    double total = 0.0;
    for (size_t v : region) {
      const bool busy_here = core_busy(core, v);
      for (const Edge& e : out_edges_[v]) {
        const double mid = is_inf(e.dwell_max) ? static_cast<double>(e.dwell_min)
                                               : 0.5 * (e.dwell_min + e.dwell_max);
        total += mid;
        if (busy_here) {
          busy += mid;
        }
      }
    }
    cm.graph_busy_fraction = total > 0.0 ? busy / total : 0.0;
    report.cores.push_back(std::move(cm));
  }
  std::sort(report.cores.begin(), report.cores.end(),
            [](const CoreMetrics& a, const CoreMetrics& b) { return a.core < b.core; });
}

}  // namespace state_class
