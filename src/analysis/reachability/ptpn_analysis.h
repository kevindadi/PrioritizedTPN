#ifndef ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H
#define ANALYSIS_REACHABILITY_PTPN_ANALYSIS_H

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "analysis/reachability/canonicalization.h"
#include "analysis/reachability/scheduling/firing_rule.h"
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

  // Replaces the policy rule. The constructor installs
  // make_firing_rule(net.schedule_policy(), net), so ordinary callers never need to
  // touch this; it exists for tests and future dynamic policies.
  void set_firing_rule(std::unique_ptr<FiringRule> rule);

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

  // Exact completion observation (metrics only). When enabled and the firing
  // rule supports it, the layout carries a scheduling-irrelevant release clock
  // for every in-flight job (EDF/FIFO already have one) and every completion
  // edge records the sup of that clock in its `observed_response`: the
  // worst-case response time of the completing job, measured from its release.
  //
  // The observation clocks are capped at the task's deadline + 1 (or a global
  // structural cap for deadline-less tasks) so a starved job cannot make the
  // state space infinite. A response below the cap is exact; a response that
  // reaches the cap is only a lower bound and the metrics layer reports it as
  // capped. The observation never changes scheduling decisions.
  void set_completion_observation(bool enabled);

  [[nodiscard]] bool completion_observation() const {
    return completion_observation_;
  }

  // True when observation clocks are actually part of the layout (requested and
  // supported by the firing rule).
  [[nodiscard]] bool observation_active() const;

  [[nodiscard]] const SCGraph& get_graph() const {
    return graph_;
  }

  [[nodiscard]] SCGraph& get_graph() {
    return graph_;
  }

  // First initial class. Kept for compatibility with callers that only need a
  // representative; use get_initial_vertices() when the analysis must cover
  // every admissible initial active set.
  [[nodiscard]] SCVertex get_initial_vertex() const {
    return initial_vertex_;
  }

  // Every initial class created by build(), after symmetry deduplication. A
  // nondeterministic initial active set (equal-priority transitions, EDF/FIFO/
  // LLF ties) yields one class per admissible branch, so consumers that reason
  // about releases or paths must consider all of them.
  [[nodiscard]] const std::vector<SCVertex>& get_initial_vertices() const {
    return initial_vertices_;
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
  // Scheduling policy semantics (priority + admissibility). Defaults to the
  // static rule derived from net_.schedule_policy().
  std::unique_ptr<FiringRule> firing_rule_;
  SCGraph graph_;
  SCVertex initial_vertex_ = 0;
  std::vector<SCVertex> initial_vertices_;
  Statistics stats_;
  CanonicalizationMode mode_ = CanonicalizationMode::EQUALITY;
  ReductionMode reduction_mode_ = ReductionMode::NONE;
  bool extrapolation_enabled_ = false;
  bool suspension_clock_projection_ = true;
  bool completion_observation_ = false;
  int extrapolation_k_ = -1;  // computed lazily from the net's static intervals
  bool horizon_enabled_ = false;
  int horizon_ = 0;
  bool symmetry_enabled_ = false;
  std::unique_ptr<TaskSymmetry> symmetry_;
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

  // True when the clock layout includes release clocks: the rule uses them for
  // scheduling (EDF/FIFO) or the completion observation requested them.
  [[nodiscard]] bool include_release_clocks() const;
  // True when any per-job clock (release or laxity) is part of the layout.
  // Replaces FiringRule::uses_job_clocks() in the engine: observation clocks
  // need the same prepared-state and layout handling.
  [[nodiscard]] bool job_clock_layout() const;
  // Observation cap for `task` (deadline + 1, or the global structural cap), or
  // INF_TIME when the task must not be capped.
  [[nodiscard]] int observation_cap(size_t task) const;
  // Observed response of the job that `transition` moves into its exit place,
  // read from the fired zone (elapsed zone intersected with h_t >= downSI(t)).
  // INF_TIME when no task completes or the response is unbounded.
  [[nodiscard]] int observe_completion(const StateClass& elapsed, size_t transition,
                                       const petri::Marking& target_marking) const;

  // Task chains of the net, cached for the LLF rate law and laxity reset.
  [[nodiscard]] const std::vector<TaskChain>& task_chains() const;
  // transition -> task ordinal for execution segments only (chain positions
  // after the zero-time get_core dispatch), or -1. Cached.
  [[nodiscard]] const std::vector<int>& execution_task_of_transition() const;
  // Exact chi = TaskInfo::wcet - deadline at a fresh release, validated to fit
  // in int. Only valid for nets that carry task metadata.
  [[nodiscard]] int laxity_reset_value(size_t task) const;

  mutable std::vector<TaskChain> task_chains_cache_;
  mutable bool task_chains_valid_ = false;
  mutable std::vector<int> execution_task_of_transition_;
  mutable bool execution_task_valid_ = false;

  // Fills struct_enabled / priority_enabled / suspended from state.marking and
  // the selected active set `active` (which is E_pri-hat(M)).
  void recompute_sets(StateClass& state, const TransitionSet& active) const;
  // Builds clock_vars and the per-transition index maps from the sets.
  void build_layout(StateClass& state) const;
  // Builds a successor zone by carrying surviving clocks over from `fired`.
  void build_successor_zone(StateClass& successor, const DBM& fired, const StateClass& source,
                            size_t fired_transition) const;
  // Keeps a transition whose execution clock is pinned at the upper bound of its
  // interval (its work is definitely done) in the active set, so a preemption
  // cannot suspend a finished clock and delay the completion. In-place and
  // bounded: it never splits a class, so it cannot multiply states.
  void force_completed_transitions(StateClass& state) const;

  // Replaces `state` by its canonical orbit representative when symmetry is
  // active. No-op otherwise.
  void canonicalize_state(StateClass& state) const;
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
