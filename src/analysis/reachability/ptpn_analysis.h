#ifndef ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H
#define ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "analysis/reachability/canonicalization.h"
#include "analysis/reachability/state_class.h"
#include "analysis/reachability/symmetry.h"
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

  // Enables the conservative task-symmetry quotient (default off). Every state
  // class is replaced by the lexicographically smallest representative of its
  // orbit under the verified task permutations before it is indexed, so the
  // graph stores one vertex per symmetry class. The exhaustive active-set
  // semantics are unchanged: admissibility is only canonicalized, never
  // filtered. Construction is cheap when the net has no admissible task
  // permutation; see TaskSymmetry for the supported scope.
  void set_symmetry_reduction(bool enabled);
  [[nodiscard]] bool get_symmetry_reduction() const {
    return symmetry_enabled_;
  }
  // True when symmetry was requested and the net admits a non-trivial verified
  // task permutation group (i.e. the built graph is actually reduced).
  [[nodiscard]] bool symmetry_applied() const;
  [[nodiscard]] size_t symmetry_group_order() const;
  // Task-name groups proven interchangeable by net automorphisms.
  [[nodiscard]] const std::vector<std::vector<std::string>>& symmetry_task_groups() const;

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
  int extrapolation_k_ = -1;  // computed lazily from the net's static intervals
  bool symmetry_enabled_ = false;
  std::unique_ptr<TaskSymmetry> symmetry_;
  size_t next_id_ = 0;

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

  // Replaces `state` by its canonical orbit representative when symmetry is
  // active. No-op otherwise.
  void canonicalize_state(StateClass& state) const;
  // Returns the vertex matching `state` under the current mode, or npos.
  [[nodiscard]] bool find_match(const StateClass& state, SCVertex& match) const;
  SCVertex add_state(StateClass state);
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H
