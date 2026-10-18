#include "analysis/metrics/metrics.h"

#include "analysis/dbm/clock_state.h"

namespace state_class {

std::string TimeValue::to_string() const {
  return infinite ? std::string("inf") : std::to_string(value);
}

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

}  // namespace state_class
