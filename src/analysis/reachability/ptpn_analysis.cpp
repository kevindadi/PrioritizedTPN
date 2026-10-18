#include "analysis/reachability/ptpn_analysis.h"

#include <algorithm>
#include <spdlog/spdlog.h>

#include "analysis/dbm/clock_state.h"

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
