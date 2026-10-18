#ifndef ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H
#define ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Sound one-sided reduction of the state-class graph. It is deliberately
// separate from CanonicalizationMode: canonicalization controls how equality of
// whole classes is decided, while a reduction may additionally merge a class
// into a larger one and therefore changes the meaning of the timing metrics.
enum class ReductionMode {
  NONE,         // keep every class that is not matched exactly
  UPPER_BOUND,  // merge a candidate into an existing class whose zone contains it
};

// Builds the state-class reachability graph of a P-TPN following the symbolic
// construction in unconfirmed/ptpn-formal-semantics.tex: time elapse on a joint
// DBM, then a branch for every priority-enabled transition that can fire.
class StateClassReachabilityGraph {
 public:
  explicit StateClassReachabilityGraph(const petri::PTPN& net);

  void set_canonicalization_mode(CanonicalizationMode mode);
  [[nodiscard]] CanonicalizationMode get_canonicalization_mode() const;

  // Enables the sound one-sided zone-merging reduction. When
  // reduction == UPPER_BOUND, a candidate successor with the same marking and
  // selected active set as an already-discovered class is merged into that
  // class if candidate.zone is included in existing.zone. Only the (larger)
  // existing class is kept, so the resulting graph over-approximates the exact
  // state space: WCRT computed from it is a sound upper bound. Lower bounds
  // derived from the same graph are not certified because the candidate's more
  // precise zone (and its possibly tighter dwell bounds) is discarded.
  //
  // The reduction takes precedence over the canonicalization mode. Classes
  // with different markings or different selected active sets are never
  // merged.
  void set_reduction_mode(ReductionMode mode);
  [[nodiscard]] ReductionMode get_reduction_mode() const;

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

  // Projects the suspension clocks w_t out of the analysis state (default:
  // enabled). The `suspended` set is retained, so suspended transitions' frozen
  // execution clocks keep exactly their old semantics. w_t occurs in no firing
  // guard, deadline cap or metric, so dropping it preserves reachability, WCRT,
  // WCET and deadlock while reducing the DBM dimension.
  void set_suspension_clock_projection(bool enabled);

  [[nodiscard]] bool get_suspension_clock_projection() const {
    return suspension_clock_projection_;
  }

  // WCRT time-horizon reduction (opt-in, default off).
  //
  // When enabled, exploration tracks the minimum accumulated lower-bound dwell
  // of every state class (sum of FiringEdge::dwell_min along the best path).
  // Classes whose minimum exceeds `horizon` are still discovered and keep their
  // incoming edges, but are not expanded further. Since dwell_min is a sound
  // lower bound on the wall-clock time spent reaching a class, no path that
  // fits within the horizon is lost. This is sound for WCRT bounds only when
  // `horizon` is an upper bound on the response time of interest; pruning
  // invalidates deadlock/SCC conclusions, which the statistics flag via
  // `horizon_truncated`.
  void set_time_horizon(int horizon);

  [[nodiscard]] bool has_time_horizon() const {
    return horizon_enabled_;
  }

  [[nodiscard]] int time_horizon() const {
    return horizon_;
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
    // ReductionMode::UPPER_BOUND only: candidates merged into an existing
    // class because their zone was included in the existing one.
    size_t zone_merges = 0;
    // Number of distinct existing classes that absorbed at least one candidate.
    // zone_merges >= merged_states when a class absorbs several candidates.
    size_t merged_states = 0;
    // Stopped at the max_states cap.
    bool truncated = false;
    // Horizon reduction bookkeeping. `horizon_pruned_states` counts discovered
    // classes that were not expanded because their minimum accumulated
    // lower-bound dwell exceeded the horizon.
    size_t horizon_pruned_states = 0;
    bool horizon_truncated = false;
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
  ReductionMode reduction_mode_ = ReductionMode::NONE;
  bool extrapolation_enabled_ = false;
  bool suspension_clock_projection_ = true;
  int extrapolation_k_ = -1;  // computed lazily from the net's static intervals
  bool horizon_enabled_ = false;
  int horizon_ = 0;
  size_t next_id_ = 0;

  // Key for UPPER_BOUND mode: classes that may merge must agree on the marking
  // and on the selected active set. The remaining layout (clock variables,
  // suspended set) is a function of those two, and check_inclusion verifies it
  // again before merging.
  struct MarkingActiveKey {
    std::vector<int> marking;
    TransitionSet active;

    bool operator==(const MarkingActiveKey& other) const {
      return marking == other.marking && active == other.active;
    }
  };

  struct MarkingActiveHash {
    size_t operator()(const MarkingActiveKey& key) const;
  };

  std::unordered_map<std::vector<int>, std::vector<SCVertex>, MarkingHash> vertices_by_marking_;
  // Equality mode only: exact-identity hash -> candidate vertices. Collisions
  // are resolved with check_equality, so lookup is O(1) expected instead of a
  // linear scan over every zone variant of the same marking.
  std::unordered_map<size_t, std::vector<SCVertex>> vertices_by_hash_;
  // UPPER_BOUND mode only: (marking, selected active set) -> vertices.
  std::unordered_map<MarkingActiveKey, std::vector<SCVertex>, MarkingActiveHash>
      vertices_by_marking_and_active_;
  // UPPER_BOUND mode only: existing vertices that absorbed at least one
  // candidate, used to count `Statistics::merged_states`.
  std::unordered_set<SCVertex> merge_targets_;

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

  // One successor produced while expanding a vertex. `is_new` distinguishes
  // freshly inserted classes from deduplicated targets so callers can seed the
  // per-vertex time label exactly once.
  struct ExpansionRecord {
    SCVertex target;
    FiringEdge edge;
    bool is_new;
  };

  // Fires every enabled transition of `source`, adds the resulting edges (and
  // new vertices) to the graph, and returns one record per successor edge.
  std::vector<ExpansionRecord> expand_vertex(SCVertex source, size_t max_states);
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H
