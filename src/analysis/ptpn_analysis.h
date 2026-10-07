#ifndef ANALYSIS_PTPN_ANALYSIS_H
#define ANALYSIS_PTPN_ANALYSIS_H

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "analysis/canonicalization.h"
#include "analysis/state_class.h"
#include "petri/petri.h"

namespace state_class {

// Boost graph where each vertex carries a symbolic state class and each edge
// records which transition fired.
typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS,
                              boost::property<boost::vertex_name_t, StateClass>,
                              boost::property<boost::edge_name_t, FiringEdge>>
    SCGraph;

typedef boost::graph_traits<SCGraph>::vertex_descriptor SCVertex;
typedef boost::graph_traits<SCGraph>::edge_descriptor SCEdge;

#ifdef PTPN_ENABLE_TEST_ACCESS
struct StateClassReachabilityGraphTestAccess;
#endif

// Builds the state-class reachability graph of a P-TPN following the symbolic
// construction in unconfirmed/ptpn-formal-semantics.tex: time elapse on a joint
// DBM, then a branch for every priority-enabled transition that can fire.
class StateClassReachabilityGraph {
#ifdef PTPN_ENABLE_TEST_ACCESS
  friend struct StateClassReachabilityGraphTestAccess;
#endif

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

  // The initial state class C0 = (M0, Omega0) with every clock pinned to zero.
  StateClass compute_initial_class();

  // TimeElapse operator: returns a copy of `state` whose zone has had time
  // pushed forward (running clocks released and re-capped at their deadlines).
  StateClass time_elapse(const StateClass& state) const;

  // True when transition `t` admits a valuation h_t >= downSI(t) in `elapsed`.
  bool is_firable(const StateClass& elapsed, size_t t) const;

  // Discrete firing: from the time-elapsed class, fire `t` and produce the
  // successor class (marking, sets, layout and zone). Returns false if the
  // firing domain is empty.
  bool fire(const StateClass& elapsed, size_t t, StateClass& successor) const;

  struct Statistics {
    size_t total_states = 0;
    size_t total_transitions = 0;
    size_t dedup_hits = 0;
    bool truncated = false;
  };

  [[nodiscard]] const Statistics& get_statistics() const {
    return stats_;
  }

  bool save_to_dot(const std::string& file_path) const;

 private:
  const petri::PTPN& net_;
  SCGraph graph_;
  SCVertex initial_vertex_ = 0;
  Statistics stats_;
  CanonicalizationMode mode_ = CanonicalizationMode::EQUALITY;
  bool extrapolation_enabled_ = false;
  int extrapolation_k_ = -1;  // computed lazily from the net's static intervals
  size_t next_id_ = 0;

  std::unordered_map<std::vector<int>, std::vector<SCVertex>, MarkingHash> vertices_by_marking_;
  // Equality mode only: exact-identity hash -> candidate vertices. Collisions
  // are resolved with check_equality, so lookup is O(1) expected instead of a
  // linear scan over every zone variant of the same marking.
  std::unordered_map<size_t, std::vector<SCVertex>> vertices_by_hash_;

  [[nodiscard]] int effective_earliest(size_t transition) const;
  [[nodiscard]] int effective_latest(size_t transition) const;

  // Fills struct_enabled / priority_enabled / suspended from state.marking.
  void recompute_sets(StateClass& state) const;
  // Builds clock_vars and the per-transition index maps from the sets.
  void build_layout(StateClass& state) const;
  // Builds a successor zone by carrying surviving clocks over from `fired`.
  void build_successor_zone(StateClass& successor, const DBM& fired, const StateClass& source,
                            size_t fired_transition) const;

  // Returns the vertex matching `state` under the current mode, or npos.
  [[nodiscard]] bool find_match(const StateClass& state, SCVertex& match) const;
  SCVertex add_state(StateClass state);

  static std::string format_marking(const petri::PTPN& net, const std::vector<int>& marking);
  std::string format_transition_label(size_t transition_id) const;
  std::string format_transitions(const TransitionSet& transitions) const;
  std::string format_named_dbm(const StateClass& state) const;
  std::string format_state_dump(const StateClass& state) const;
  // Human-readable local clock zone as a conjunction of DBM constraints (per
  // clock bounds plus non-trivial differences). No global timestamp: a state
  // class is a symbolic set, so only the symbolic clock domain is shown.
  std::vector<std::string> format_zone_constraints(const StateClass& state, bool html) const;
  // Graphviz HTML-like node label: black identity (state id / marking / enabled
  // sets) and the local clock zone with h-clocks and w-clocks colour-coded.
  std::string format_state_label_html(const StateClass& state) const;
};

}  // namespace state_class

#endif  // ANALYSIS_PTPN_ANALYSIS_H
