#include "analysis/metrics.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <numeric>

#include "analysis/clock_state.h"

namespace state_class {

namespace {

bool is_inf(int v) {
  return v == INF_TIME;
}

long long lcm_ll(long long a, long long b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  const long long g = std::gcd(a, b);
  return a / g * b;
}

}  // namespace

std::string TimeValue::to_string() const {
  return infinite ? std::string("inf") : std::to_string(value);
}

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

MetricsAnalyzer::MetricsAnalyzer(const SCGraph& graph, const petri::PTPN& net, SCVertex initial,
                                 bool exact)
    : graph_(graph), net_(net), initial_(initial), exact_(exact) {
  flatten_graph();
  build_topology();
}

void MetricsAnalyzer::flatten_graph() {
  num_vertices_ = boost::num_vertices(graph_);
  out_edges_.assign(num_vertices_, {});
  state_of_.assign(num_vertices_, nullptr);

  typedef boost::graph_traits<SCGraph>::vertex_iterator VIt;
  VIt vi, vi_end;
  for (std::tie(vi, vi_end) = boost::vertices(graph_); vi != vi_end; ++vi) {
    const StateClass& s = boost::get(boost::vertex_name, graph_, *vi);
    state_of_[s.id] = &s;
  }

  typedef boost::graph_traits<SCGraph>::edge_iterator EIt;
  EIt ei, ei_end;
  for (std::tie(ei, ei_end) = boost::edges(graph_); ei != ei_end; ++ei) {
    const SCVertex src = boost::source(*ei, graph_);
    const SCVertex tgt = boost::target(*ei, graph_);
    const FiringEdge& fe = boost::get(boost::edge_name, graph_, *ei);
    const size_t sid = boost::get(boost::vertex_name, graph_, src).id;
    const size_t tid = boost::get(boost::vertex_name, graph_, tgt).id;
    Edge e;
    e.target = tid;
    e.transition_id = fe.transition_id;
    e.dwell_min = fe.dwell_min;
    e.dwell_max = fe.dwell_max;
    out_edges_[sid].push_back(e);
  }
}

void MetricsAnalyzer::build_topology() {
  transition_task_.assign(net_.num_transitions(), -1);
  transition_is_exec_.assign(net_.num_transitions(), false);
  place_lock_.assign(net_.num_places(), "");
  for (const auto& [lock_name, place_idx] : net_.lock_places()) {
    if (place_idx.index() < place_lock_.size()) {
      place_lock_[place_idx.index()] = lock_name;
    }
  }

  for (const auto& [name, info] : net_.task_info()) {
    const auto chain_it = net_.node_pn_map().find(name);
    if (chain_it == net_.node_pn_map().end() || chain_it->second.empty()) {
      continue;
    }
    const std::vector<petri::NodeRef>& chain = chain_it->second;

    TaskTopology topo;
    topo.name = name;
    topo.core = info.core;
    topo.priority = info.priority;

    // The chain strictly alternates place, transition, place, ...; NodeRef
    // records the kind, so a mismatched fragment fails loudly here.
    const int task_index = static_cast<int>(tasks_.size());
    for (const petri::NodeRef& node : chain) {
      if (node.is_place()) {
        topo.chain_places.push_back(node.as_place().index());
        continue;
      }
      const size_t t = node.as_transition().index();
      topo.chain_transitions.push_back(t);
      if (t < transition_task_.size()) {
        transition_task_[t] = task_index;
      }
      if (t < net_.num_transitions() &&
          net_.get_transition(petri::TransitionId{t}).name.find("exec") != std::string::npos) {
        topo.exec_transitions.push_back(t);
        if (t < transition_is_exec_.size()) {
          transition_is_exec_[t] = true;
        }
      }
    }
    topo.entry_place = chain.front().as_place().index();
    topo.end_place = chain.back().as_place().index();
    topo.has_end = true;

    tasks_.push_back(std::move(topo));
  }
}

bool MetricsAnalyzer::task_in_flight(const TaskTopology& task, size_t v) const {
  const std::vector<int>& m = state_of_[v]->marking;
  for (size_t p : task.chain_places) {
    if (p < m.size() && m[p] > 0) {
      return true;
    }
  }
  return false;
}

bool MetricsAnalyzer::task_active(const TaskTopology& task, size_t v) const {
  const TransitionSet& active = state_of_[v]->priority_enabled;
  for (size_t t : task.chain_transitions) {
    if (contains(active, t)) {
      return true;
    }
  }
  return false;
}

bool MetricsAnalyzer::task_suspended(const TaskTopology& task, size_t v) const {
  const TransitionSet& susp = state_of_[v]->suspended;
  for (size_t t : task.exec_transitions) {
    if (contains(susp, t)) {
      return true;
    }
  }
  return false;
}

bool MetricsAnalyzer::core_busy(int core, size_t v) const {
  if (core < 0) {
    return false;
  }
  for (size_t t : state_of_[v]->priority_enabled) {
    if (t < transition_is_exec_.size() && transition_is_exec_[t] &&
        net_.get_transition(petri::TransitionId{t}).core == core) {
      return true;
    }
  }
  return false;
}

bool MetricsAnalyzer::task_blocked_by_lower(const TaskTopology& task, size_t v) const {
  if (task.core < 0) {
    return false;
  }
  if (!task_in_flight(task, v) || task_active(task, v)) {
    return false;
  }
  // Priority inversion: a strictly lower-priority task executes on this core.
  for (size_t t : state_of_[v]->priority_enabled) {
    if (t >= transition_is_exec_.size() || !transition_is_exec_[t]) {
      continue;
    }
    if (net_.get_transition(petri::TransitionId{t}).core != task.core) {
      continue;
    }
    if (net_.get_transition(petri::TransitionId{t}).priority < task.priority) {
      return true;
    }
  }
  return false;
}

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

MetricsReport MetricsAnalyzer::analyze() {
  MetricsReport report;
  report.exact = exact_;
  report.states = num_vertices_;
  size_t edge_count = 0;
  for (const auto& edges : out_edges_) {
    edge_count += edges.size();
  }
  report.transitions = edge_count;

  compute_structural(report);
  compute_schedulability(report);
  compute_task_timing(report);
  compute_locks(report);
  compute_utilisation(report);
  return report;
}

namespace {

std::string time_json(const TimeValue& t) {
  return t.infinite ? std::string("null") : std::to_string(t.value);
}

}  // namespace

bool MetricsAnalyzer::save_to_json(const MetricsReport& report, const std::string& file_path) {
  std::ofstream out(file_path);
  if (!out.is_open()) {
    return false;
  }

  out << "{\n";
  out << "  \"exact\": " << (report.exact ? "true" : "false") << ",\n";
  out << "  \"states\": " << report.states << ",\n";
  out << "  \"transitions\": " << report.transitions << ",\n";
  out << "  \"bounded\": " << (report.bounded ? "true" : "false") << ",\n";
  out << "  \"schedulable\": " << (report.schedulable ? "true" : "false") << ",\n";
  out << "  \"has_steady_cycle\": " << (report.has_steady_cycle ? "true" : "false") << ",\n";
  out << "  \"recurrent_scc_size\": " << report.recurrent_scc_size << ",\n";
  out << "  \"hyperperiod\": " << report.hyperperiod << ",\n";

  out << "  \"deadlock_states\": [";
  for (size_t i = 0; i < report.deadlock_states.size(); ++i) {
    if (i)
      out << ", ";
    out << report.deadlock_states[i];
  }
  out << "],\n";

  out << "  \"tasks\": [\n";
  for (size_t i = 0; i < report.tasks.size(); ++i) {
    const TaskMetrics& t = report.tasks[i];
    out << "    {\n";
    out << "      \"name\": \"" << t.name << "\",\n";
    out << "      \"core\": " << t.core << ",\n";
    out << "      \"priority\": " << t.priority << ",\n";
    out << "      \"wcet\": " << t.wcet << ",\n";
    out << "      \"bcet\": " << t.bcet << ",\n";
    out << "      \"period\": " << t.period << ",\n";
    out << "      \"deadline\": " << t.deadline << ",\n";
    out << "      \"observed\": " << (t.observed ? "true" : "false") << ",\n";
    out << "      \"activations\": " << t.activations << ",\n";
    out << "      \"wcrt\": " << time_json(t.wcrt) << ",\n";
    out << "      \"bcrt\": " << time_json(t.bcrt) << ",\n";
    out << "      \"jitter\": " << time_json(t.jitter) << ",\n";
    out << "      \"worst_interference\": " << time_json(t.worst_interference) << ",\n";
    out << "      \"worst_blocking\": " << time_json(t.worst_blocking) << ",\n";
    out << "      \"max_preemptions\": " << t.max_preemptions << ",\n";
    out << "      \"max_in_flight\": " << t.max_in_flight << ",\n";
    out << "      \"slack\": " << (t.has_deadline ? time_json(t.slack) : std::string("null"))
        << ",\n";
    out << "      \"jobs_per_hyperperiod\": " << t.jobs_per_hyperperiod << "\n";
    out << "    }" << (i + 1 < report.tasks.size() ? "," : "") << "\n";
  }
  out << "  ],\n";

  out << "  \"locks\": [\n";
  for (size_t i = 0; i < report.locks.size(); ++i) {
    const LockMetrics& l = report.locks[i];
    out << "    {\n";
    out << "      \"name\": \"" << l.name << "\",\n";
    out << "      \"worst_hold\": " << time_json(l.worst_hold) << ",\n";
    out << "      \"total_hold\": " << time_json(l.total_hold) << ",\n";
    out << "      \"total_wait\": " << time_json(l.total_wait) << "\n";
    out << "    }" << (i + 1 < report.locks.size() ? "," : "") << "\n";
  }
  out << "  ],\n";

  out << "  \"cores\": [\n";
  for (size_t i = 0; i < report.cores.size(); ++i) {
    const CoreMetrics& c = report.cores[i];
    out << "    {\n";
    out << "      \"core\": " << c.core << ",\n";
    out << "      \"util_min\": " << c.util_min << ",\n";
    out << "      \"util_max\": " << c.util_max << ",\n";
    out << "      \"graph_busy_fraction\": " << c.graph_busy_fraction << "\n";
    out << "    }" << (i + 1 < report.cores.size() ? "," : "") << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return true;
}

}  // namespace state_class
