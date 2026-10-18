#include <algorithm>

#include "analysis/dbm/clock_state.h"
#include "analysis/reachability/ptpn_analysis.h"

namespace state_class {

namespace {

// Sorted-vector membership for small core-id lists.
bool contains_core(const std::vector<int>& sorted, int value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

bool intersects(const std::vector<size_t>& left, const std::vector<size_t>& right) {
  size_t i = 0;
  size_t j = 0;
  while (i < left.size() && j < right.size()) {
    if (left[i] == right[j]) {
      return true;
    }
    if (left[i] < right[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return false;
}

}  // namespace

void StateClassReachabilityGraph::set_por_enabled(bool enabled) {
  por_enabled_ = enabled;
}

void StateClassReachabilityGraph::build_task_chain_places() {
  task_chain_places_.clear();
  for (const auto& [name, info] : net_.task_info()) {
    (void)info;
    const auto chain_it = net_.node_pn_map().find(name);
    if (chain_it == net_.node_pn_map().end()) {
      continue;
    }
    for (const petri::NodeRef& node : chain_it->second) {
      if (node.is_place()) {
        task_chain_places_.push_back(node.as_place().index());
      }
    }
  }
  std::sort(task_chain_places_.begin(), task_chain_places_.end());
  task_chain_places_.erase(
      std::unique(task_chain_places_.begin(), task_chain_places_.end()),
      task_chain_places_.end());
}

bool StateClassReachabilityGraph::has_task_in_flight(const StateClass& state) const {
  for (size_t place : task_chain_places_) {
    if (place < state.marking.size() && state.marking[place] > 0) {
      return true;
    }
  }
  return false;
}

void StateClassReachabilityGraph::build_dependency_relation() {
  const size_t num_transitions = net_.num_transitions();
  const size_t num_places = net_.num_places();
  dependency_.assign(num_transitions, std::vector<unsigned char>(num_transitions, 0));
  if (num_transitions == 0) {
    return;
  }

  // Places touched by a transition (pre U post), sorted and duplicate-free.
  // Two transitions sharing an entry here conflict through the marking and are
  // dependent. Shared lock places are a special case of this rule: the
  // lowering records a lock as one resource place, so acquire/release
  // transitions on the same lock share that place.
  std::vector<std::vector<size_t>> touched(num_transitions);
  // Real cores that consume a place as input. Used by the conservative
  // schedule-interference rule.
  std::vector<std::vector<int>> consuming_real_cores(num_places);

  for (size_t t = 0; t < num_transitions; ++t) {
    for (const auto& arc : net_.pre_arcs()[t]) {
      touched[t].push_back(arc.first);
    }
    for (const auto& arc : net_.post_arcs()[t]) {
      touched[t].push_back(arc.first);
    }
    std::sort(touched[t].begin(), touched[t].end());
    touched[t].erase(std::unique(touched[t].begin(), touched[t].end()), touched[t].end());

    const int core = net_.get_transition(petri::TransitionId{t}).core;
    if (core < 0) {
      continue;
    }
    for (const auto& arc : net_.pre_arcs()[t]) {
      std::vector<int>& cores = consuming_real_cores[arc.first];
      cores.push_back(core);
    }
  }
  for (std::vector<int>& cores : consuming_real_cores) {
    std::sort(cores.begin(), cores.end());
    cores.erase(std::unique(cores.begin(), cores.end()), cores.end());
  }

  for (size_t a = 0; a < num_transitions; ++a) {
    dependency_[a][a] = 1;
    const int core_a = net_.get_transition(petri::TransitionId{a}).core;
    for (size_t b = a + 1; b < num_transitions; ++b) {
      const int core_b = net_.get_transition(petri::TransitionId{b}).core;

      bool dependent = false;
      // Same real core: the two transitions compete for one CPU slot (equal
      // priority ties included), so their firing order is a scheduling
      // decision that must be preserved. The control core (-1) is not
      // CPU-scheduled: its transitions are concurrently active and are only
      // dependent through places or schedule interference.
      if (core_a >= 0 && core_a == core_b) {
        dependent = true;
      }
      // Shared pre/post place (also covers task/control dependencies and
      // shared lock places).
      if (!dependent && intersects(touched[a], touched[b])) {
        dependent = true;
      }
      // Schedule interference: firing a touches a place consumed by some
      // transition on b's real core (or vice versa). Installing a token that a
      // core can observe is conservatively treated as interference even when
      // a and b share no place directly.
      if (!dependent && core_b >= 0) {
        for (size_t place : touched[a]) {
          if (contains_core(consuming_real_cores[place], core_b)) {
            dependent = true;
            break;
          }
        }
      }
      if (!dependent && core_a >= 0) {
        for (size_t place : touched[b]) {
          if (contains_core(consuming_real_cores[place], core_a)) {
            dependent = true;
            break;
          }
        }
      }

      dependency_[a][b] = dependent ? 1 : 0;
      dependency_[b][a] = dependent ? 1 : 0;
    }
  }
}

bool StateClassReachabilityGraph::transitions_dependent(size_t a, size_t b) const {
  if (a >= dependency_.size() || b >= dependency_.size()) {
    return false;
  }
  return dependency_[a][b] != 0;
}

TransitionSet StateClassReachabilityGraph::compute_persistent_set(
    const StateClass& state) const {
  const StateClass elapsed = time_elapse(state);
  return persistent_set_for(state, elapsed, state.priority_enabled);
}

TransitionSet StateClassReachabilityGraph::persistent_set_for(
    const StateClass& current, const StateClass& elapsed,
    const TransitionSet& enabled) const {
  if (enabled.size() <= 1) {
    return enabled;
  }

  const size_t num_transitions = net_.num_transitions();
  std::vector<unsigned char> in_set(num_transitions, 0);
  TransitionSet persistent;

  auto add = [&](size_t transition) {
    if (transition < num_transitions && in_set[transition] == 0) {
      in_set[transition] = 1;
      persistent.push_back(transition);
    }
  };

  // Deterministic seed: the first enabled transition that can actually fire
  // in this class (falling back to the first enabled transition when none
  // can). Seeding with a non-firable transition would drop the firable ones
  // whenever the dependency closure is small.
  size_t seed = enabled.front();
  for (size_t t : enabled) {
    if (is_firable(elapsed, t)) {
      seed = t;
      break;
    }
  }
  add(seed);

  auto close = [&]() {
    for (size_t head = 0; head < persistent.size(); ++head) {
      const size_t a = persistent[head];
      for (size_t b : enabled) {
        if (in_set[b] == 0 && transitions_dependent(a, b)) {
          add(b);
        }
      }
    }
  };
  close();

  // Timed POR: a transition at the minimum effective latest must fire before
  // time can advance further, so it cannot be commuted behind another
  // transition. If every enabled transition is unbounded there is no urgent
  // transition and the condition contributes nothing.
  int urgent_latest = INF_TIME;
  for (size_t t : enabled) {
    const int latest = effective_latest(t);
    if (latest != INF_TIME) {
      urgent_latest = std::min(urgent_latest, latest);
    }
  }
  if (urgent_latest != INF_TIME) {
    for (size_t t : enabled) {
      if (effective_latest(t) == urgent_latest) {
        add(t);
      }
    }
    close();
  }

  // Metric preservation. The timing analysis computes per-task worst-case
  // response times as longest paths over per-edge dwell maxima. An edge that
  // can consume time therefore carries weight into some task's path whenever a
  // task can be in flight in its source class, and commuting that edge behind
  // an independent transition can change the reported interval. Keep every
  // firable transition with a positive dwell in such classes; transitions
  // outside every task's execution window, and zero-dwell setup/control edges,
  // remain eligible for reduction.
  if (has_task_in_flight(current)) {
    for (size_t t : enabled) {
      if (in_set[t] != 0 || !is_firable(elapsed, t)) {
        continue;
      }
      const int hcidx = current.exec_index(t);
      const size_t eidx = static_cast<size_t>(elapsed.exec_index(t));
      const int h_low = -elapsed.zone.get_constraint(0, eidx);
      const int h_high = elapsed.zone.get_constraint(eidx, 0);
      const int fire_min = std::max({0, effective_earliest(t), h_low});
      int fire_max = h_high;
      const int up = effective_latest(t);
      if (up != INF_TIME && (fire_max == INF_TIME || up < fire_max)) {
        fire_max = up;
      }
      if (fire_min > fire_max) {
        continue;
      }
      int dwell_max = fire_max;
      if (hcidx > 0) {
        const int entry_low = -current.zone.get_constraint(0, static_cast<size_t>(hcidx));
        dwell_max = (fire_max == INF_TIME || entry_low == INF_TIME)
                        ? INF_TIME
                        : std::max(0, fire_max - entry_low);
      }
      if (dwell_max > 0) {
        add(t);
      }
    }
    close();
  }

  std::sort(persistent.begin(), persistent.end());
  if (persistent.size() == enabled.size()) {
    // No reduction: keep the original (full) enabled set.
    return enabled;
  }
  return persistent;
}

}  // namespace state_class
