#include "analysis/ptpn_analysis.h"

#include <algorithm>
#include <spdlog/spdlog.h>

#include "analysis/clock_state.h"
#include "analysis/scheduling.h"

namespace state_class {

StateClassReachabilityGraph::StateClassReachabilityGraph(const petri::PTPN& net) : net_(net) {}

void StateClassReachabilityGraph::set_canonicalization_mode(CanonicalizationMode mode) {
  mode_ = mode;
}

void StateClassReachabilityGraph::set_extrapolation(bool enabled) {
  extrapolation_enabled_ = enabled;
  if (!enabled) {
    return;
  }
  // Uniform bound k: the largest finite endpoint of any static interval. Any
  // clock value beyond k can never influence a future firing decision.
  int k = 0;
  for (size_t t = 0; t < net_.num_transitions(); ++t) {
    k = std::max(k, effective_earliest(t));
    const int latest = effective_latest(t);
    if (latest != INF_TIME) {
      k = std::max(k, latest);
    }
  }
  extrapolation_k_ = k;
}

CanonicalizationMode StateClassReachabilityGraph::get_canonicalization_mode() const {
  return mode_;
}

int StateClassReachabilityGraph::effective_earliest(size_t transition) const {
  return net_.get_transition(petri::TransitionId{transition}).time_interval.effective_earliest();
}

int StateClassReachabilityGraph::effective_latest(size_t transition) const {
  const int latest =
      net_.get_transition(petri::TransitionId{transition}).time_interval.effective_latest();
  return latest == petri::INF ? INF_TIME : latest;
}

void StateClassReachabilityGraph::recompute_sets(StateClass& state) const {
  state.struct_enabled = Scheduling::structural_enabled(net_, state.marking);
  state.priority_enabled = Scheduling::filter_priority_per_core(state.struct_enabled, net_);

  state.suspended.clear();
  // struct_enabled is sorted, so suspended stays sorted too.
  for (size_t t : state.struct_enabled) {
    if (contains(state.priority_enabled, t)) {
      continue;
    }
    if (net_.get_transition(petri::TransitionId{t}).suspendable) {
      state.suspended.push_back(t);
    }
  }
}

void StateClassReachabilityGraph::build_layout(StateClass& state) const {
  const size_t num_transitions = net_.num_transitions();

  state.clock_vars.clear();
  state.clock_vars.push_back({ClockKind::Zero, NO_TRANSITION});
  state.exec_clock_of_transition.assign(num_transitions, -1);
  state.susp_clock_of_transition.assign(num_transitions, -1);

  for (size_t t : state.struct_enabled) {
    const int exec_idx = static_cast<int>(state.clock_vars.size());
    state.clock_vars.push_back({ClockKind::Execution, t});
    state.exec_clock_of_transition[t] = exec_idx;

    if (contains(state.suspended, t)) {
      const int susp_idx = static_cast<int>(state.clock_vars.size());
      state.clock_vars.push_back({ClockKind::Suspension, t});
      state.susp_clock_of_transition[t] = susp_idx;
    }
  }
}

StateClass StateClassReachabilityGraph::compute_initial_class() {
  StateClass state;
  state.marking = net_.get_marking();
  state.elapsed_time = 0.0;

  recompute_sets(state);
  build_layout(state);

  // Every clock starts at zero, i.e. pinned to the reference variable x0.
  const size_t n = state.clock_vars.size();
  DBM zone(n);
  for (size_t i = 1; i < n; ++i) {
    zone.set_constraint(0, i, 0);
    zone.set_constraint(i, 0, 0);
  }
  zone.minimize();
  state.zone = std::move(zone);

  return state;
}

StateClass StateClassReachabilityGraph::time_elapse(const StateClass& state) const {
  StateClass out = state;
  const size_t n = out.zone.size();
  if (n == 0) {
    return out;
  }

  // Running clocks advance at rate 1; everything else (x0 and the frozen
  // execution clocks of priority-filtered-out transitions) stays put.
  // V_run = { h_t : t in E_pri } U { w_t : t in suspended }.
  std::vector<bool> running(n, false);
  for (size_t i = 1; i < n && i < out.clock_vars.size(); ++i) {
    const ClockVar& var = out.clock_vars[i];
    if (var.kind == ClockKind::Suspension) {
      running[i] = true;  // suspension clocks always advance
    } else if (var.kind == ClockKind::Execution && contains(out.priority_enabled, var.transition)) {
      running[i] = true;  // active execution clocks advance
    }
  }

  // Release each running clock's upper bound relative to every stationary
  // variable (x0 and the frozen clocks). The differences between two running
  // clocks are left untouched, so they keep growing synchronously.
  for (size_t i = 1; i < n; ++i) {
    if (!running[i]) {
      continue;
    }
    for (size_t j = 0; j < n; ++j) {
      if (j != i && !running[j]) {
        out.zone.set_constraint(i, j, INF_TIME);
      }
    }
  }

  // Strong time semantics: cap each active execution clock at its deadline
  // upSI(t) so time cannot advance past a transition that must fire.
  for (size_t t : out.priority_enabled) {
    if (!out.has_exec_clock(t)) {
      continue;
    }
    const int upper = effective_latest(t);
    if (upper == INF_TIME) {
      continue;
    }
    const size_t idx = static_cast<size_t>(out.exec_index(t));
    const int current = out.zone.get_constraint(idx, 0);
    if (current == INF_TIME || upper < current) {
      out.zone.set_constraint(idx, 0, upper);
    }
  }

  out.zone.minimize();
  return out;
}

bool StateClassReachabilityGraph::is_firable(const StateClass& elapsed, size_t t) const {
  if (!contains(elapsed.priority_enabled, t) || !elapsed.has_exec_clock(t)) {
    return false;
  }

  const size_t idx = static_cast<size_t>(elapsed.exec_index(t));
  const int max_h = elapsed.zone.get_constraint(idx, 0);  // largest feasible h_t
  if (max_h == INF_TIME) {
    return true;
  }
  return max_h >= effective_earliest(t);
}

void StateClassReachabilityGraph::build_successor_zone(StateClass& successor, const DBM& fired,
                                                       const StateClass& source,
                                                       size_t fired_transition) const {
  const size_t n = successor.clock_vars.size();
  DBM zone(n);

  // For each successor variable, find the matching column in `fired` if the
  // clock survives the firing (a persistent execution clock keeps its value; a
  // still-suspended clock inherits its suspension clock). Everything else is
  // freshly created and starts at zero.
  std::vector<int> source_index(n, -1);
  source_index[0] = 0;  // x0 maps to x0
  for (size_t i = 1; i < n; ++i) {
    const ClockVar& var = successor.clock_vars[i];
    const size_t t = var.transition;
    if (var.kind == ClockKind::Execution) {
      if (t != fired_transition && source.has_exec_clock(t)) {
        source_index[i] = source.exec_index(t);
      }
    } else {  // Suspension
      if (t != fired_transition && source.has_susp_clock(t)) {
        source_index[i] = source.susp_index(t);
      }
    }
  }

  // Carry over the joint constraints between all surviving clocks.
  for (size_t i = 0; i < n; ++i) {
    if (source_index[i] < 0) {
      continue;
    }
    for (size_t j = 0; j < n; ++j) {
      if (source_index[j] < 0) {
        continue;
      }
      zone.set_constraint(i, j,
                          fired.get_constraint(static_cast<size_t>(source_index[i]),
                                               static_cast<size_t>(source_index[j])));
    }
  }

  // Pin every freshly created clock to zero (equal to x0).
  for (size_t i = 1; i < n; ++i) {
    if (source_index[i] < 0) {
      zone.set_constraint(0, i, 0);
      zone.set_constraint(i, 0, 0);
    }
  }

  zone.minimize();
  successor.zone = std::move(zone);
}

bool StateClassReachabilityGraph::fire(const StateClass& elapsed, size_t t,
                                       StateClass& successor) const {
  if (!is_firable(elapsed, t)) {
    return false;
  }

  // Step 1: intersect the firing-domain constraint h_t >= downSI(t). The
  // elapsed zone is already canonical, so a single-constraint incremental
  // tightening (O(n^2)) replaces the full closure.
  DBM fired = elapsed.zone;
  const size_t idx = static_cast<size_t>(elapsed.exec_index(t));
  const int lower = effective_earliest(t);
  if (!fired.tighten(0, idx, -lower)) {
    return false;
  }

  // Step 2: discrete token shuffle.
  successor = StateClass();
  successor.marking = petri::PTPN::fire(elapsed.marking, net_, petri::TransitionId{t});

  // Steps 3 & 4: recompute the scheduler sets and the variable layout, then
  // rebuild the zone carrying surviving clocks over from `fired`.
  recompute_sets(successor);
  build_layout(successor);
  build_successor_zone(successor, fired, elapsed, t);

  const int h_lower = -elapsed.zone.get_constraint(0, idx);
  const int firing_instant = std::max(lower, h_lower);
  successor.elapsed_time = elapsed.elapsed_time + std::max(0, firing_instant);

  return true;
}

bool StateClassReachabilityGraph::find_match(const StateClass& state, SCVertex& match) const {
  if (mode_ == CanonicalizationMode::EQUALITY) {
    auto it = vertices_by_hash_.find(hash_state_class(state));
    if (it == vertices_by_hash_.end()) {
      return false;
    }
    for (SCVertex candidate : it->second) {
      const StateClass& existing = boost::get(boost::vertex_name, graph_, candidate);
      if (check_equality(state, existing)) {
        match = candidate;
        return true;
      }
    }
    return false;
  }

  auto it = vertices_by_marking_.find(state.marking);
  if (it == vertices_by_marking_.end()) {
    return false;
  }
  for (SCVertex candidate : it->second) {
    const StateClass& existing = boost::get(boost::vertex_name, graph_, candidate);
    if (can_merge_into(state, existing, mode_)) {
      match = candidate;
      return true;
    }
  }
  return false;
}

SCVertex StateClassReachabilityGraph::add_state(StateClass state) {
  state.id = next_id_++;
  if (mode_ == CanonicalizationMode::EQUALITY) {
    const size_t hash = hash_state_class(state);
    SCVertex v = boost::add_vertex(std::move(state), graph_);
    vertices_by_hash_[hash].push_back(v);
    return v;
  }
  const std::vector<int> marking = state.marking;
  SCVertex v = boost::add_vertex(std::move(state), graph_);
  vertices_by_marking_[marking].push_back(v);
  return v;
}

size_t StateClassReachabilityGraph::build(size_t max_states) {
  graph_.clear();
  vertices_by_marking_.clear();
  vertices_by_hash_.clear();
  stats_ = Statistics();
  next_id_ = 0;
  reset_dbm_instrumentation();
  petri::reset_overflow_recording();

  StateClass initial = compute_initial_class();
  if (extrapolation_enabled_) {
    initial.zone.extrapolate(extrapolation_k_);
  }
  initial_vertex_ = add_state(std::move(initial));
  stats_.total_states = 1;

  std::vector<SCVertex> frontier{initial_vertex_};

  while (!frontier.empty()) {
    std::vector<SCVertex> next_frontier;

    for (SCVertex u : frontier) {
      if (stats_.total_states >= max_states) {
        stats_.truncated = true;
        break;
      }

      // Extract everything needed from the source state up front instead of
      // deep-copying the whole StateClass: with vecS vertex storage, the
      // reference returned by boost::get is invalidated by add_vertex, so no
      // reference into the graph may be held across add_state below.
      struct EntryBounds {
        bool has_clock = false;
        int low = 0;
        int high = 0;
      };

      std::vector<size_t> enabled;
      std::vector<EntryBounds> entry_bounds;
      StateClass elapsed;
      {
        const StateClass& current = boost::get(boost::vertex_name, graph_, u);
        elapsed = time_elapse(current);
        enabled.assign(current.priority_enabled.begin(), current.priority_enabled.end());
        entry_bounds.resize(enabled.size());
        // The entry range of h_t comes from the pre-elapse zone (same layout,
        // same index as the elapsed zone).
        for (size_t k = 0; k < enabled.size(); ++k) {
          const int hcidx = current.exec_index(enabled[k]);
          if (hcidx > 0) {
            const size_t cidx = static_cast<size_t>(hcidx);
            entry_bounds[k] = {true, -current.zone.get_constraint(0, cidx),
                               current.zone.get_constraint(cidx, 0)};
          }
        }
      }

      for (size_t k = 0; k < enabled.size(); ++k) {
        const size_t t = enabled[k];
        if (!is_firable(elapsed, t)) {
          continue;
        }

        StateClass successor;
        if (!fire(elapsed, t, successor)) {
          continue;
        }
        if (extrapolation_enabled_) {
          successor.zone.extrapolate(extrapolation_k_);
        }

        // The real firing window of h_t: intersect its feasible range in the
        // time-elapsed zone with the static interval [downSI, upSI].
        const size_t hidx = static_cast<size_t>(elapsed.exec_index(t));
        const int h_low = -elapsed.zone.get_constraint(0, hidx);
        const int h_high = elapsed.zone.get_constraint(hidx, 0);
        const int up = effective_latest(t);
        const int fire_min = std::max({0, effective_earliest(t), h_low});
        int fire_max = h_high;
        if (up != INF_TIME && (fire_max == INF_TIME || up < fire_max)) {
          fire_max = up;
        }

        // Global dwell in the source class before this firing. h_t advances at
        // rate 1 during the dwell, so dwell = fire_value - h_t(entry).
        int dwell_min = fire_min;
        int dwell_max = fire_max;
        if (entry_bounds[k].has_clock) {
          const int entry_low = entry_bounds[k].low;
          const int entry_high = entry_bounds[k].high;
          dwell_min = std::max(0, fire_min - entry_high);
          dwell_max = (fire_max == INF_TIME || entry_low == INF_TIME)
                          ? INF_TIME
                          : std::max(0, fire_max - entry_low);
        }
        FiringEdge edge(static_cast<int>(t), fire_min, fire_max);
        edge.dwell_min = dwell_min;
        edge.dwell_max = dwell_max;

        SCVertex v;
        if (find_match(successor, v)) {
          stats_.dedup_hits++;
          boost::add_edge(u, v, edge, graph_);
          stats_.total_transitions++;
          continue;
        }

        if (stats_.total_states >= max_states) {
          stats_.truncated = true;
          continue;
        }

        v = add_state(std::move(successor));
        stats_.total_states++;
        boost::add_edge(u, v, edge, graph_);
        stats_.total_transitions++;
        next_frontier.push_back(v);
      }

      if (stats_.truncated) {
        break;
      }
    }

    if (stats_.truncated) {
      break;
    }
    frontier = std::move(next_frontier);
  }

  spdlog::info(
      "[SCG] build complete: states={}, transitions={}, dedup_hits={}, "
      "truncated={}, dbm_minimize_calls={}",
      stats_.total_states, stats_.total_transitions, stats_.dedup_hits,
      stats_.truncated ? "true" : "false", get_dbm_instrumentation().minimize_calls);

  return stats_.total_states;
}

}  // namespace state_class
