#ifndef ANALYSIS_REACHABILITY_STATE_CLASS_H
#define ANALYSIS_REACHABILITY_STATE_CLASS_H

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "analysis/dbm/clock_state.h"
#include "analysis/dbm/dbm.h"
#include "model/petri.h"

namespace state_class {

constexpr size_t NO_TRANSITION = std::numeric_limits<size_t>::max();
constexpr size_t NO_TASK = std::numeric_limits<size_t>::max();

// Sorted vector of transition ids used as a lightweight ordered set: far more
// compact and copy-friendly than std::set for the small, read-mostly enabling
// sets carried by every state class.
using TransitionSet = std::vector<size_t>;

inline bool contains(const TransitionSet& set, size_t value) {
  return std::binary_search(set.begin(), set.end(), value);
}

// Every column/row of the joint DBM corresponds to one timed variable.
enum class ClockKind {
  Zero,        // the reference variable x0 (always lives at index 0)
  Release,     // r_i: time since the current job of task i was released
  Laxity,      // chi_i: urgency (-laxity) of the current job of task i
  Execution,   // h_t: how long transition t has effectively been running
  Suspension,  // w_t: how long suspendable transition t has been suspended
};

// Maps a DBM column index back to the variable it represents.
//
// `transition` identifies Execution/Suspension clocks; `task` identifies
// Release/Laxity clocks (an ordinal into the name-sorted task list produced by
// build_task_chains). Exactly one of the two is meaningful for a non-Zero
// clock; the unused field is NO_TRANSITION/NO_TASK.
struct ClockVar {
  ClockKind kind = ClockKind::Zero;
  size_t transition = NO_TRANSITION;
  size_t task = NO_TASK;

  bool operator==(const ClockVar& other) const {
    return kind == other.kind && transition == other.transition && task == other.task;
  }

  bool operator<(const ClockVar& other) const {
    if (kind != other.kind)
      return kind < other.kind;
    if (transition != other.transition)
      return transition < other.transition;
    return task < other.task;
  }
};

// Static description of one task chain, derived from the lowered net
// (`node_pn_map` + `task_info` + `periodic_transitions`). The list returned by
// build_task_chains is sorted by task name; the position in that list is the
// task ordinal used by ClockVar::task and StateClass::release_clock_of_task.
struct TaskChain {
  std::string name;
  // Real core the task is statically bound to (TaskInfo::core). All chain
  // transitions of the task run on this core; control transitions use -1.
  int core = -1;
  int deadline = 0;
  int period = 0;
  // Worst-case execution time from TaskInfo: the sum of the execution-segment
  // upper bounds. Used by Event-driven LLF to reset chi = wcet - deadline.
  int wcet = 0;
  size_t entry_place = NO_TRANSITION;
  size_t exit_place = NO_TRANSITION;
  size_t periodic_transition = NO_TRANSITION;
  std::vector<petri::NodeRef> chain;
  std::vector<size_t> places;
  std::vector<size_t> transitions;
};

// Task chains of every task that has both a chain in node_pn_map and TaskInfo,
// sorted by name (stable ordinals for the analysis layer).
std::vector<TaskChain> build_task_chains(const petri::PTPN& net);

// Number of tokens in the task chain, including the exit (completion) place.
size_t task_chain_token_count(const TaskChain& task, const std::vector<int>& marking);

// A job is in flight from its release (a token in the entry place) until its
// final execution segment places a token in the exit place. Exit tokens that
// await the end-of-task consumer are completed jobs and do not carry a release
// clock.
bool task_in_flight(const TaskChain& task, const std::vector<int>& marking);

// One edge of the state-class graph: which transition fired, plus the feasible
// firing window of its execution clock h_t at the moment it fires. The window
// [firing_min, firing_max] is the real (symbolic) timing; firing_max may be
// +infinity (INF_TIME).
//
// [dwell_min, dwell_max] is the global time the net may spend in the SOURCE
// state class before this firing (the amount every active clock advances). It is
// the per-edge time weight all timing metrics build on; dwell_max may be
// +infinity (INF_TIME).
struct FiringEdge {
  int transition_id = -1;
  int firing_min = 0;
  int firing_max = 0;
  int dwell_min = 0;
  int dwell_max = 0;
  // Completion observation (release-clock metrics): when the fired transition
  // moves a task's job token into its exit place, this is the sup of that
  // task's observation release clock in the fired zone, i.e. the worst-case
  // response time of the completing job along this edge. INF_TIME means "not
  // observed" or "unbounded"; the metrics pass reads it only on completion
  // edges while the graph carries observation clocks.
  int observed_response = INF_TIME;

  FiringEdge() = default;

  FiringEdge(int id, int window_min, int window_max)
      : transition_id(id), firing_min(window_min), firing_max(window_max) {}

  FiringEdge(int id, int window_min, int window_max, int stay_min, int stay_max)
      : transition_id(id),
        firing_min(window_min),
        firing_max(window_max),
        dwell_min(stay_min),
        dwell_max(stay_max) {}

  bool operator==(const FiringEdge& other) const {
    return transition_id == other.transition_id && firing_min == other.firing_min &&
           firing_max == other.firing_max && dwell_min == other.dwell_min &&
           dwell_max == other.dwell_max && observed_response == other.observed_response;
  }

  [[nodiscard]] std::string to_string() const;
};

// Symbolic state class C = (M, Omega) together with the scheduler-facing sets
// that the priority semantics needs. The DBM `zone` ranges over the dynamic
// variable vector X = {x0} U {h_t : t in struct_enabled}
//                       U {w_t : t in suspended}.
struct StateClass {
  std::vector<int> marking;  // M
  DBM zone;                  // Omega

  std::vector<ClockVar> clock_vars;           // index -> variable; [0] is Zero
  std::vector<int> exec_clock_of_transition;  // transition -> h_t index or -1
  std::vector<int> susp_clock_of_transition;  // transition -> w_t index or -1
  // Task ordinal (name-sorted build_task_chains order) -> r_task index or -1.
  // Present only when release-clock tracking is enabled (EDF/FIFO). A task has
  // a release clock exactly while it has an in-flight job.
  std::vector<int> release_clock_of_task;
  // Task ordinal -> chi_task index or -1. Present only when laxity-clock
  // tracking is enabled (Event-driven LLF). A task has a laxity clock exactly
  // while it has an in-flight job.
  std::vector<int> laxity_clock_of_task;

  TransitionSet struct_enabled;    // E_struct(M)
  TransitionSet priority_enabled;  // E_pri-hat(M): selected active transitions
  TransitionSet suspended;         // (E_struct \ E_pri-hat) intersect T2

  double elapsed_time = 0.0;  // auxiliary metadata, excluded from identity
  size_t id = 0;              // assigned when inserted into the graph

  [[nodiscard]] int exec_index(size_t transition) const {
    return transition < exec_clock_of_transition.size() ? exec_clock_of_transition[transition] : -1;
  }

  [[nodiscard]] int susp_index(size_t transition) const {
    return transition < susp_clock_of_transition.size() ? susp_clock_of_transition[transition] : -1;
  }

  [[nodiscard]] bool has_exec_clock(size_t transition) const {
    return exec_index(transition) > 0;
  }

  [[nodiscard]] bool has_susp_clock(size_t transition) const {
    return susp_index(transition) > 0;
  }

  [[nodiscard]] int release_index(size_t task) const {
    return task < release_clock_of_task.size() ? release_clock_of_task[task] : -1;
  }

  [[nodiscard]] bool has_release_clock(size_t task) const {
    return release_index(task) > 0;
  }

  [[nodiscard]] int laxity_index(size_t task) const {
    return task < laxity_clock_of_task.size() ? laxity_clock_of_task[task] : -1;
  }

  [[nodiscard]] bool has_laxity_clock(size_t task) const {
    return laxity_index(task) > 0;
  }
};

// True iff the active rule tracks per-job clocks (release clocks and/or laxity
// clocks). Such states reject extrapolation and symmetry reduction, whose
// quotient/extrapolation arguments are only sound for the transition-indexed
// clock layouts of static/PIP/PCP/SRP rules.
bool uses_job_clock_kind(ClockKind kind);

// Column index of the per-job clock of `kind` for `task`, or -1 when absent.
inline int job_clock_index(const StateClass& state, ClockKind kind, size_t task) {
  switch (kind) {
    case ClockKind::Release:
      return state.release_index(task);
    case ClockKind::Laxity:
      return state.laxity_index(task);
    default:
      return -1;
  }
}

// Completion-observation cap for one task: the deadline + 1, or the period + 1
// when the task has no explicit deadline. A job whose release clock reaches the
// cap has already missed its deadline, so the clock is clamped there and the
// state space stays finite. INF_TIME means "no deadline and no period": the
// caller falls back to fallback_observation_cap(). Shared by the graph's clock
// clamp and the metrics layer's capped-value detection so both agree on the
// exact cap.
inline int task_observation_cap(int deadline, int period) {
  const int bound = deadline > 0 ? deadline : period;
  if (bound <= 0 || bound >= std::numeric_limits<int>::max() - 1) {
    return INF_TIME;
  }
  return bound + 1;
}

// Net-wide fallback cap for tasks with neither a deadline nor a period. Such a
// task may be starved forever, so no structural bound exists; the cap only has
// to keep the release-clock column finite while staying above any response a
// schedulable-ish system can produce. Heuristic: twice the total execution
// demand plus the largest finite transition bound. INF_TIME when the net has
// no finite bound and no task metadata. Both the graph's clock clamp and the
// metrics layer's capped-value detection call this so they agree bit for bit.
inline int fallback_observation_cap(const petri::PTPN& net) {
  long long total_wcet = 0;
  for (const auto& entry : net.task_info()) {
    total_wcet += std::max(0, entry.second.wcet);
  }
  int max_bound = 0;
  for (size_t t = 0; t < net.num_transitions(); ++t) {
    const petri::TimeInterval& interval =
        net.get_transition(petri::TransitionId{t}).time_interval;
    const int latest = interval.effective_latest();
    if (latest != petri::INF) {
      max_bound = std::max(max_bound, latest);
    }
    max_bound = std::max(max_bound, interval.effective_earliest());
  }
  if (max_bound <= 0 && total_wcet <= 0) {
    return INF_TIME;
  }
  const long long cap = 2 * total_wcet + std::max(1, max_bound);
  if (cap >= static_cast<long long>(std::numeric_limits<int>::max()) - 1) {
    return INF_TIME;
  }
  return static_cast<int>(cap);
}

// Human-readable single-letter name used in diagnostics and DOT labels.
const char* clock_kind_label(ClockKind kind);

// Rebuilds clock_vars and the per-transition/per-task clock index maps from
// struct_enabled/suspended/marking. The layout is canonical:
//
//   x0,
//   one job clock per in-flight task (ascending task ordinal, only when
//   `include_release_clocks` or `include_laxity_clocks` is set; the two kinds
//   are mutually exclusive),
//   one execution clock per structurally enabled transition in ascending
//   order, followed by its suspension clock when the transition is suspended
//   and `keep_suspension_clocks` is set.
//
// Job clocks live immediately after x0, so their indices are independent of
// the selected active set (and of whether suspension clocks are projected).
// Shared by the symbolic construction and the symmetry canonicalizer so both
// agree bit for bit.
void build_clock_layout(StateClass& state, const petri::PTPN& net,
                        bool keep_suspension_clocks = false, bool include_release_clocks = false,
                        bool include_laxity_clocks = false);

// Hashes the marking alone so candidates that might merge land in one bucket.
struct MarkingHash {
  size_t operator()(const std::vector<int>& marking) const;
};

// Exact-identity hash over (marking, scheduler sets, clock_vars, zone matrix)
// used for O(1) deduplication in equality mode. The selected active set is part
// of the identity: two classes may share marking, layout and zone but differ in
// which clocks are allowed to run, and they are different semantic states.
// Collisions are resolved with check_equality().
size_t hash_state_class(const StateClass& state);

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_STATE_CLASS_H
