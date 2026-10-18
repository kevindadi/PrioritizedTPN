#ifndef ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H
#define ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "analysis/reachability/canonicalization.h"
#include "analysis/reachability/state_class.h"
#include "model/petri.h"

namespace state_class {

// Boost graph where each vertex carries a symbolic state class and each edge
// records which transition fired.
typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS,
                              boost::property<boost::vertex_name_t, StateClass>,
                              boost::property<boost::edge_name_t, FiringEdge>>
    SCGraph;

typedef boost::graph_traits<SCGraph>::vertex_descriptor SCVertex;
typedef boost::graph_traits<SCGraph>::edge_descriptor SCEdge;

// Builds the state-class reachability graph of a P-TPN following the symbolic
// construction in unconfirmed/ptpn-formal-semantics.tex: time elapse on a joint
// DBM, then a branch for every priority-enabled transition that can fire.
class StateClassReachabilityGraph {
 public:
  explicit StateClassReachabilityGraph(const petri::PTPN& net);

  void set_canonicalization_mode(CanonicalizationMode mode);
  [[nodiscard]] CanonicalizationMode get_canonicalization_mode() const;

  // Enables k-extrapolation of successor zones (opt-in). The bound k is the
  // largest finite constant of any transition's static interval; zone bounds
  // beyond k are relaxed, which merges behaviorally equivalent state classes
  // and keeps the zone space finite. Preserves the reachable marking set.
  void set_extrapolation(bool enabled);

  [[nodiscard]] bool get_extrapolation() const {
    return extrapolation_enabled_;
  }

  [[nodiscard]] int extrapolation_bound() const {
    return extrapolation_k_;
  }

  // Opt-in partial-order reduction (persistent sets). Disabled by default: the
  // exhaustive active-set semantics and the full exploration path are retained
  // when it is off. When enabled, each state class explores only a persistent
  // set of its priority-enabled transitions; same-core transitions and every
  // edge that carries positive dwell into a task's execution window stay in
  // the set, so the reported WCET, WCRT interval and deadlocks are unchanged.
  void set_por_enabled(bool enabled);
  [[nodiscard]] bool get_por_enabled() const {
    return por_enabled_;
  }

  // True when firing `a` and `b` interfere. The relation is symmetric and is
  // precomputed once from the net: shared pre/post place (which covers shared
  // lock places), same real core (core >= 0, including equal-priority ties),
  // or schedule interference through a place consumed on the other
  // transition's real core.
  [[nodiscard]] bool transitions_dependent(size_t a, size_t b) const;

  // Persistent set of `state`: the dependency closure of a deterministic seed
  // over the priority-enabled transitions, extended with every urgent
  // transition (minimum finite effective latest), closed again, and finally
  // enlarged with every firable transition whose edge carries a positive dwell
  // while a task is in flight. That last step keeps the per-task WCRT upper
  // bounds of the timing analysis unchanged. Returns the full priority-enabled
  // set when no reduction is possible.
  [[nodiscard]] TransitionSet compute_persistent_set(const StateClass& state) const;

  // Explores the reachability graph, stopping once `max_states` classes exist.
  // Returns the number of state classes discovered.
  size_t build(size_t max_states = std::numeric_limits<size_t>::max());

  [[nodiscard]] const SCGraph& get_graph() const {
    return graph_;
  }

  [[nodiscard]] SCGraph& get_graph() {
    return graph_;
  }

  [[nodiscard]] SCVertex get_initial_vertex() const {
    return initial_vertex_;
  }

  // The initial state classes C0 = (M0, Omega0) with every clock pinned to
  // zero, one per admissible selected active set. There is deliberately no
  // deterministic single-branch variant: equal-priority candidates are all
  // reachable scheduling choices and must be preserved. `compute_initial_class`
  // returns the unique class and throws when the initial marking admits more
  // than one active set.
  std::vector<StateClass> compute_initial_classes();
  StateClass compute_initial_class();

  // TimeElapse operator: returns a copy of `state` whose zone has had time
  // pushed forward (running clocks released and re-capped at their deadlines).
  StateClass time_elapse(const StateClass& state) const;

  // True when transition `t` admits a valuation h_t >= downSI(t) in `elapsed`.
  bool is_firable(const StateClass& elapsed, size_t t) const;

  // Discrete firing: from the time-elapsed class, fire `t` and produce every
  // successor class, one per admissible target active set. `fire` is a
  // single-successor convenience that throws when the firing is ambiguous.
  bool fire(const StateClass& elapsed, size_t t, StateClass& successor) const;
  std::vector<StateClass> fire_all(const StateClass& elapsed, size_t t) const;

  struct Statistics {
    size_t total_states = 0;
    size_t total_transitions = 0;
    size_t dedup_hits = 0;
    bool truncated = false;
    // Partial-order reduction counters (zero while POR is disabled).
    size_t persistent_set_calls = 0;
    size_t persistent_transitions_explored = 0;
    size_t persistent_transitions_skipped = 0;
    size_t max_persistent_set_size = 0;
  };

  [[nodiscard]] const Statistics& get_statistics() const {
    return stats_;
  }

 private:
  const petri::PTPN& net_;
  SCGraph graph_;
  SCVertex initial_vertex_ = 0;
  Statistics stats_;
  CanonicalizationMode mode_ = CanonicalizationMode::EQUALITY;
  bool extrapolation_enabled_ = false;
  bool por_enabled_ = false;
  int extrapolation_k_ = -1;  // computed lazily from the net's static intervals
  size_t next_id_ = 0;

  // Symmetric transition-dependency matrix, built once in the constructor.
  // dependency_[a][b] != 0 means firing a and b are not timed-independent.
  std::vector<std::vector<unsigned char>> dependency_;
  void build_dependency_relation();

  // Union of the task chain places. The timing analysis only charges an
  // edge's dwell to a task when the source class holds a token of that task's
  // chain, so dwell outside these places cannot influence any reported WCRT.
  std::vector<size_t> task_chain_places_;
  void build_task_chain_places();
  [[nodiscard]] bool has_task_in_flight(const StateClass& state) const;

  // Core of the persistent-set algorithm: `enabled` is sorted, `current` is
  // the stored class and `elapsed` is its time-elapsed expansion. The result
  // includes the metric-preservation additions described in the .cpp file.
  [[nodiscard]] TransitionSet persistent_set_for(const StateClass& current,
                                                 const StateClass& elapsed,
                                                 const TransitionSet& enabled) const;

  std::unordered_map<std::vector<int>, std::vector<SCVertex>, MarkingHash> vertices_by_marking_;
  // Equality mode only: exact-identity hash -> candidate vertices. Collisions
  // are resolved with check_equality, so lookup is O(1) expected instead of a
  // linear scan over every zone variant of the same marking.
  std::unordered_map<size_t, std::vector<SCVertex>> vertices_by_hash_;

  [[nodiscard]] int effective_earliest(size_t transition) const;
  [[nodiscard]] int effective_latest(size_t transition) const;

  // Fills struct_enabled / priority_enabled / suspended from state.marking and
  // the selected active set `active` (which is E_pri-hat(M)).
  void recompute_sets(StateClass& state, const TransitionSet& active) const;
  // Builds clock_vars and the per-transition index maps from the sets.
  void build_layout(StateClass& state) const;
  // Builds a successor zone by carrying surviving clocks over from `fired`.
  void build_successor_zone(StateClass& successor, const DBM& fired, const StateClass& source,
                            size_t fired_transition) const;

  // Returns the vertex matching `state` under the current mode, or npos.
  [[nodiscard]] bool find_match(const StateClass& state, SCVertex& match) const;
  SCVertex add_state(StateClass state);
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H
