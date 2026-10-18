#include "analysis/reachability/ptpn_analysis.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

#include "analysis/dbm/clock_state.h"

namespace state_class {

namespace {
void hash_combine(size_t& seed, size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}
}  // namespace

StateClassReachabilityGraph::StateClassReachabilityGraph(const petri::PTPN& net) : net_(net) {
  set_firing_rule(make_firing_rule(net.schedule_policy(), net));
}

void StateClassReachabilityGraph::set_firing_rule(std::unique_ptr<FiringRule> rule) {
  if (!rule) {
    throw std::invalid_argument("firing rule must not be null");
  }
  if (extrapolation_enabled_ && rule->uses_job_clocks()) {
    throw std::invalid_argument(
        "k-extrapolation is not sound for job-clock (EDF/FIFO/LLF) states; disable "
        "extrapolation before installing this rule");
  }
  firing_rule_ = std::move(rule);
}

void StateClassReachabilityGraph::set_canonicalization_mode(CanonicalizationMode mode) {
  mode_ = mode;
}

void StateClassReachabilityGraph::set_suspension_clock_projection(bool enabled) {
  suspension_clock_projection_ = enabled;
}

void StateClassReachabilityGraph::set_reduction_mode(ReductionMode mode) {
  reduction_mode_ = mode;
}

ReductionMode StateClassReachabilityGraph::get_reduction_mode() const {
  return reduction_mode_;
}

size_t StateClassReachabilityGraph::MarkingActiveHash::operator()(
    const MarkingActiveKey& key) const {
  size_t seed = MarkingHash{}(key.marking);
  hash_combine(seed, key.active.size());
  for (size_t t : key.active) {
    hash_combine(seed, std::hash<size_t>{}(t));
  }
  return seed;
}

void StateClassReachabilityGraph::set_extrapolation(bool enabled) {
  if (enabled && firing_rule_ && firing_rule_->uses_job_clocks()) {
    throw std::invalid_argument(
        "k-extrapolation is not sound for job-clock (EDF/FIFO/LLF) states: relaxing "
        "job-clock bounds can merge different scheduling orderings");
  }
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

void StateClassReachabilityGraph::set_time_horizon(int horizon) {
  if (horizon < 0) {
    throw std::invalid_argument("time horizon must be non-negative");
  }
  horizon_enabled_ = true;
  horizon_ = horizon;
}

void StateClassReachabilityGraph::set_symmetry_reduction(bool enabled) {
  symmetry_enabled_ = enabled;
  symmetry_.reset();
  if (!enabled) {
    return;
  }
  symmetry_ = std::make_unique<TaskSymmetry>(net_);
  spdlog::info(
      "[SCG] Symmetry reduction requested: group_order={}, task_groups={}, "
      "applied={}",
      symmetry_->group_order(), symmetry_->task_groups().size(),
      symmetry_->has_nontrivial_group() ? "true" : "false");
  if (!symmetry_->has_nontrivial_group()) {
    spdlog::info(
        "[SCG] No verified permutations of isomorphic tasks on distinct real "
        "cores; graph is not reduced");
  }
}

bool StateClassReachabilityGraph::symmetry_applied() const {
  return symmetry_ != nullptr && symmetry_->has_nontrivial_group();
}

size_t StateClassReachabilityGraph::symmetry_group_order() const {
  return symmetry_ != nullptr ? symmetry_->group_order() : 1;
}

const std::vector<std::vector<std::string>>& StateClassReachabilityGraph::symmetry_task_groups()
    const {
  static const std::vector<std::vector<std::string>> kEmpty;
  return symmetry_ != nullptr ? symmetry_->task_groups() : kEmpty;
}

void StateClassReachabilityGraph::canonicalize_state(StateClass& state) const {
  if (symmetry_ != nullptr && symmetry_->has_nontrivial_group()) {
    state = symmetry_->canonicalize(std::move(state));
  }
}

bool StateClassReachabilityGraph::find_match(const StateClass& state, SCVertex& match) const {
  if (reduction_mode_ == ReductionMode::UPPER_BOUND) {
    // One-sided merging: a candidate is absorbed only when an already
    // discovered class with the same marking and selected active set has a
    // zone that contains the candidate's zone. `check_inclusion` re-verifies
    // the full layout (clock variables and suspended set) before merging.
    auto it = vertices_by_marking_and_active_.find(
        MarkingActiveKey{state.marking, state.priority_enabled});
    if (it == vertices_by_marking_and_active_.end()) {
      return false;
    }
    for (SCVertex candidate : it->second) {
      const StateClass& existing = boost::get(boost::vertex_name, graph_, candidate);
      if (check_inclusion(state, existing)) {
        match = candidate;
        return true;
      }
    }
    return false;
  }

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
  if (reduction_mode_ == ReductionMode::UPPER_BOUND) {
    MarkingActiveKey key{state.marking, state.priority_enabled};
    SCVertex v = boost::add_vertex(std::move(state), graph_);
    vertices_by_marking_and_active_[std::move(key)].push_back(v);
    return v;
  }
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

std::vector<StateClassReachabilityGraph::ExpansionRecord>
StateClassReachabilityGraph::expand_vertex(SCVertex u, size_t max_states) {
  std::vector<ExpansionRecord> records;

  // Extract everything needed from the source state up front instead of
  // deep-copying the whole StateClass: with vecS vertex storage, the reference
  // returned by boost::get is invalidated by add_vertex, so no reference into
  // the graph may be held across add_state below.
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
    // The entry range of h_t comes from the pre-elapse zone (same layout, same
    // index as the elapsed zone).
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

    // One firing yields one successor per admissible target active set; the
    // edge weight is identical for all of them.
    std::vector<StateClass> successors = fire_all(elapsed, t);
    for (StateClass& successor : successors) {
      if (extrapolation_enabled_) {
        successor.zone.extrapolate(extrapolation_k_);
      }
      canonicalize_state(successor);

      SCVertex v;
      if (find_match(successor, v)) {
        stats_.dedup_hits++;
        if (reduction_mode_ == ReductionMode::UPPER_BOUND) {
          stats_.zone_merges++;
          if (merge_targets_.insert(v).second) {
            stats_.merged_states++;
          }
        }
        boost::add_edge(u, v, edge, graph_);
        stats_.total_transitions++;
        records.push_back({v, edge, false});
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
      records.push_back({v, edge, true});
    }
  }

  return records;
}

size_t StateClassReachabilityGraph::build(size_t max_states) {
  // Job-clock states carry per-job deadlines/laxities in the DBM. Neither
  // k-extrapolation (which would relax those bounds) nor the task-symmetry
  // quotient (which would have to permute release/laxity clocks and tasks) is
  // implemented for them; reject rather than silently approximate.
  if (firing_rule_->uses_job_clocks()) {
    if (extrapolation_enabled_) {
      throw std::invalid_argument(
          "k-extrapolation is not sound for job-clock (EDF/FIFO/LLF) states");
    }
    if (symmetry_applied()) {
      throw std::invalid_argument(
          "task-symmetry reduction is not implemented for job-clock "
          "(EDF/FIFO/LLF) states");
    }
  }
  graph_.clear();
  vertices_by_marking_.clear();
  vertices_by_hash_.clear();
  vertices_by_marking_and_active_.clear();
  merge_targets_.clear();
  stats_ = Statistics();
  next_id_ = 0;
  initial_vertex_ = 0;
  initial_vertices_.clear();
  reset_dbm_instrumentation();
  petri::reset_overflow_recording();

  std::vector<StateClass> initial_states = compute_initial_classes();
  if (initial_states.empty()) {
    return 0;
  }

  initial_vertices_.reserve(initial_states.size());
  for (StateClass& initial : initial_states) {
    canonicalize_state(initial);
    if (extrapolation_enabled_) {
      initial.zone.extrapolate(extrapolation_k_);
    }
    // Different admissible initial branches (e.g. one of two isomorphic tasks
    // being scheduled first on its own core) can share an orbit; keep one.
    // Guarded so runs without symmetry keep the previous initial-state
    // semantics exactly.
    if (symmetry_applied()) {
      SCVertex existing;
      if (find_match(initial, existing)) {
        stats_.dedup_hits++;
        continue;
      }
    }
    SCVertex v = add_state(std::move(initial));
    initial_vertices_.push_back(v);
    stats_.total_states++;
  }
  if (!initial_vertices_.empty()) {
    initial_vertex_ = initial_vertices_.front();
  }

  if (!horizon_enabled_) {
    // Exhaustive breadth-first exploration: unchanged default semantics.
    std::vector<SCVertex> frontier = initial_vertices_;
    while (!frontier.empty()) {
      std::vector<SCVertex> next_frontier;

      for (SCVertex u : frontier) {
        if (stats_.total_states >= max_states) {
          stats_.truncated = true;
          break;
        }

        for (const ExpansionRecord& record : expand_vertex(u, max_states)) {
          if (record.is_new) {
            next_frontier.push_back(record.target);
          }
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
  } else {
    // Horizon mode: explore in increasing order of accumulated lower-bound
    // dwell. With non-negative edge weights the first pop of a vertex is its
    // minimum distance, so each class is expanded at most once and no path that
    // fits in the horizon can be missed. Classes whose minimum distance exceeds
    // the horizon are left in the graph (incoming edges and all) but never
    // expanded.
    struct QueueEntry {
      long long time = 0;
      SCVertex vertex = 0;
    };

    struct LaterFirst {
      bool operator()(const QueueEntry& left, const QueueEntry& right) const {
        return left.time > right.time;
      }
    };

    std::priority_queue<QueueEntry, std::vector<QueueEntry>, LaterFirst> queue;
    const size_t initial_count = static_cast<size_t>(boost::num_vertices(graph_));
    std::vector<long long> min_time(initial_count, 0);
    std::vector<char> settled(initial_count, 0);
    for (SCVertex v : initial_vertices_) {
      queue.push({0, v});
    }

    while (!queue.empty()) {
      const QueueEntry entry = queue.top();
      queue.pop();

      const size_t u_index = static_cast<size_t>(entry.vertex);
      if (settled[u_index] != 0 || entry.time != min_time[u_index]) {
        continue;
      }
      if (stats_.total_states >= max_states) {
        stats_.truncated = true;
        break;
      }

      settled[u_index] = 1;
      if (entry.time > static_cast<long long>(horizon_)) {
        // Sound: every path to this class has accumulated at least this much
        // lower-bound dwell, so no continuation can fall back within the
        // horizon.
        stats_.horizon_pruned_states++;
        stats_.horizon_truncated = true;
        continue;
      }

      for (const ExpansionRecord& record : expand_vertex(entry.vertex, max_states)) {
        const size_t target_index = static_cast<size_t>(record.target);
        if (min_time.size() <= target_index) {
          min_time.resize(target_index + 1, 0);
          settled.resize(target_index + 1, 0);
        }
        const long long candidate = entry.time + static_cast<long long>(record.edge.dwell_min);
        if (record.is_new) {
          min_time[target_index] = candidate;
          queue.push({candidate, record.target});
        } else if (candidate < min_time[target_index]) {
          min_time[target_index] = candidate;
          queue.push({candidate, record.target});
        }
      }

      if (stats_.truncated) {
        break;
      }
    }
  }

  const std::string horizon_label =
      horizon_enabled_ ? std::to_string(horizon_) : std::string("off");
  spdlog::info(
      "[SCG] build complete: states={}, transitions={}, dedup_hits={}, "
      "zone_merges={}, merged_states={}, truncated={}, horizon={}, "
      "horizon_pruned={}, dbm_minimize_calls={}",
      stats_.total_states, stats_.total_transitions, stats_.dedup_hits, stats_.zone_merges,
      stats_.merged_states, stats_.truncated ? "true" : "false", horizon_label,
      stats_.horizon_pruned_states, get_dbm_instrumentation().minimize_calls);

  return stats_.total_states;
}

}  // namespace state_class
