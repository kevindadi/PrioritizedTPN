#include "analysis/metrics/metrics.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "analysis/dbm/clock_state.h"

namespace state_class {

namespace {

TimeValue time_max(const TimeValue& left, const TimeValue& right) {
  if (left.infinite) {
    return left;
  }
  if (right.infinite) {
    return right;
  }
  return left.value >= right.value ? left : right;
}

bool is_time_equal(const TimeValue& left, const TimeValue& right) {
  return !left.infinite && !right.infinite && left.value == right.value;
}

}  // namespace

std::string TimeValue::to_string() const {
  return infinite ? std::string("inf") : std::to_string(value);
}

MetricsAnalyzer::MetricsAnalyzer(const SCGraph& graph, const petri::PTPN& net, SCVertex initial,
                                 bool exact, bool wcrt_lb_valid)
    : MetricsAnalyzer(graph, net, std::vector<SCVertex>{initial}, exact, wcrt_lb_valid) {}

MetricsAnalyzer::MetricsAnalyzer(const SCGraph& graph, const petri::PTPN& net,
                                 const std::vector<SCVertex>& initial_vertices, bool exact,
                                 bool wcrt_lb_valid)
    : graph_(graph),
      net_(net),
      initial_vertices_(initial_vertices),
      exact_(exact),
      wcrt_lb_valid_(wcrt_lb_valid) {
  flatten_graph();
  build_topology();
}

void MetricsAnalyzer::set_horizon_info(HorizonInfo info) {
  horizon_ = info;
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

bool MetricsAnalyzer::task_pending(const TaskTopology& task, size_t v) const {
  // Pending = a token is waiting anywhere before the exit place. The exit
  // token itself means the activation already completed, so it is excluded.
  const std::vector<int>& m = state_of_[v]->marking;
  for (size_t p : task.chain_places) {
    if (p == task.end_place) {
      continue;
    }
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

void MetricsAnalyzer::set_symmetry_task_groups(
    std::vector<std::vector<std::string>> groups) {
  symmetry_task_groups_ = std::move(groups);
}

void MetricsAnalyzer::apply_symmetry_groups(MetricsReport& report) {
  if (symmetry_task_groups_.empty()) {
    return;
  }
  std::unordered_map<std::string, size_t> task_index;
  for (size_t i = 0; i < report.tasks.size(); ++i) {
    task_index[report.tasks[i].name] = i;
  }

  for (const std::vector<std::string>& group : symmetry_task_groups_) {
    std::vector<size_t> members;
    for (const std::string& name : group) {
      const auto it = task_index.find(name);
      if (it != task_index.end()) {
        members.push_back(it->second);
      }
    }
    if (members.size() < 2) {
      continue;
    }

    bool any_observed = false;
    bool any_local_deadlock = false;
    int max_activations = 0;
    int max_in_flight = 0;
    int max_preemptions = 0;
    size_t max_scc = 0;
    TimeValue pooled_wcrt;
    TimeValue pooled_wcrt_lb;
    TimeValue pooled_bcrt;
    TimeValue pooled_interference;
    TimeValue pooled_blocking;
    for (size_t member : members) {
      const TaskMetrics& task = report.tasks[member];
      any_observed = any_observed || task.observed;
      any_local_deadlock = any_local_deadlock || task.local_deadlock;
      max_activations = std::max(max_activations, task.activations);
      max_in_flight = std::max(max_in_flight, task.max_in_flight);
      max_preemptions = (max_preemptions < 0 || task.max_preemptions < 0)
                            ? -1
                            : std::max(max_preemptions, task.max_preemptions);
      max_scc = std::max(max_scc, task.local_deadlock_scc_size);
      if (!task.observed) {
        continue;
      }
      pooled_wcrt = time_max(pooled_wcrt, task.wcrt);
      pooled_wcrt_lb = time_max(pooled_wcrt_lb, task.wcrt_lb);
      pooled_bcrt = time_max(pooled_bcrt, task.bcrt);
      pooled_interference = time_max(pooled_interference, task.worst_interference);
      pooled_blocking = time_max(pooled_blocking, task.worst_blocking);
    }

    for (size_t member : members) {
      TaskMetrics& task = report.tasks[member];
      // Symmetric tasks are interchangeable, so a release observed through one
      // representative label is observable for every group member.
      task.observed = any_observed;
      task.activations = max_activations;
      task.max_in_flight = max_in_flight;
      task.max_preemptions = max_preemptions;
      task.local_deadlock = any_local_deadlock;
      task.local_deadlock_scc_size = max_scc;

      if (!any_observed) {
        continue;
      }
      task.wcrt = pooled_wcrt;
      task.wcrt_lb = pooled_wcrt_lb;
      task.bcrt = pooled_bcrt;
      task.worst_interference = pooled_interference;
      task.worst_blocking = pooled_blocking;
      task.wcrt_interval_tight = is_time_equal(pooled_wcrt, pooled_wcrt_lb);
      task.jitter.infinite = pooled_wcrt.infinite;
      if (!pooled_wcrt.infinite && !pooled_bcrt.infinite) {
        task.jitter.value =
            std::max<long long>(0, pooled_wcrt.value - pooled_bcrt.value);
      }
      if (task.has_deadline) {
        task.deadline_missed =
            pooled_wcrt.infinite || pooled_wcrt.value > task.deadline;
        task.slack.infinite = pooled_wcrt.infinite;
        if (!pooled_wcrt.infinite) {
          task.slack.value =
              static_cast<long long>(task.deadline) - pooled_wcrt.value;
        }
      }
    }
  }
}

MetricsReport MetricsAnalyzer::analyze() {
  MetricsReport report;
  report.exact = exact_;
  report.wcrt_lb_valid = wcrt_lb_valid_;
  report.horizon = horizon_;
  // A horizon-pruned graph is missing the successors of boundary classes.
  // Deadlocks, schedulability, local deadlocks and SCC statistics computed on
  // it would be artifacts of the cut, so they are skipped and flagged. WCRT
  // values are still produced, but they are approximate unless the caller's
  // horizon is a valid upper bound on the response time of interest. The
  // lower-bound endpoint is not certified on a pruned graph either.
  report.deadlock_analysis_certified = !(horizon_.enabled && horizon_.truncated);
  report.exact = exact_ && report.deadlock_analysis_certified;
  if (!report.deadlock_analysis_certified) {
    report.wcrt_lb_valid = false;
  }
  report.states = num_vertices_;
  size_t edge_count = 0;
  for (const auto& edges : out_edges_) {
    edge_count += edges.size();
  }
  report.transitions = edge_count;

  compute_structural(report);
  compute_task_timing(report);
  if (report.deadlock_analysis_certified) {
    compute_local_deadlocks(report);
    // Pool timing (and local-deadlock flags) within verified symmetry groups
    // before deadlines and schedulability are evaluated.
    apply_symmetry_groups(report);
    compute_schedulability(report);
  } else {
    // Symmetric members still share the same true timing, so pool what was
    // computed before reporting; deadlock-derived fields remain uncertified.
    apply_symmetry_groups(report);
    // Conservative: schedulability is not proven for the truncated graph.
    // `deadlock_analysis_certified` tells consumers why this is false.
    report.schedulable = false;
  }
  compute_locks(report);
  compute_utilisation(report);
  return report;
}

}  // namespace state_class
