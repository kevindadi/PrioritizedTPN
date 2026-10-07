#ifndef ANALYSIS_METRICS_METRICS_H
#define ANALYSIS_METRICS_METRICS_H

#include <string>
#include <vector>

#include "analysis/reachability/ptpn_analysis.h"
#include "model/petri.h"

namespace state_class {

// A possibly-unbounded non-negative time value. `infinite` overrides `value`.
struct TimeValue {
  long long value = 0;
  bool infinite = false;

  [[nodiscard]] std::string to_string() const;
};

// Per-task performance metrics derived from the reachability graph. All times
// are sound bounds over the symbolic state classes (see docs/ptopner/metrics.md).
struct TaskMetrics {
  std::string name;
  int core = -1;
  int priority = 0;
  int wcet = 0;
  int bcet = 0;
  int period = 0;
  int deadline = 0;

  bool observed = false;  // at least one activation seen in the graph
  int activations = 0;    // number of release events observed

  TimeValue wcrt;                // worst-case response time
  TimeValue bcrt;                // best-case response time
  TimeValue jitter;              // wcrt - bcrt
  TimeValue worst_interference;  // max time suspended by higher priority
  TimeValue worst_blocking;      // max time blocked by lower priority (inversion)
  int max_preemptions = 0;       // max active->suspended transitions per activation
  int max_in_flight = 0;         // max simultaneous tokens across this task's chain

  bool has_deadline = false;
  TimeValue slack;               // deadline - wcrt (only when has_deadline)
  int jobs_per_hyperperiod = 0;  // hyperperiod / period (0 if aperiodic)
};

// Per-lock contention metrics.
struct LockMetrics {
  std::string name;
  TimeValue worst_hold;  // longest single critical-section dwell
  TimeValue total_hold;  // total time the lock is held (graph-wide upper bound)
  TimeValue total_wait;  // total time some task waits for the held lock
};

// Per-core utilisation. `graph_busy_fraction` is an approximate state-class
// average; the [util_min, util_max] interval is the analytic C/T bound.
struct CoreMetrics {
  int core = -1;
  double util_min = 0.0;
  double util_max = 0.0;
  double graph_busy_fraction = 0.0;
};

struct MetricsReport {
  bool exact = true;  // built under EQUALITY canonicalization
  size_t states = 0;
  size_t transitions = 0;
  bool truncated = false;

  bool bounded = true;
  std::vector<int> max_tokens_per_place;     // index -> max tokens observed
  std::vector<std::string> overflow_places;  // places exceeding capacity

  std::vector<size_t> deadlock_states;  // ids of illegitimate sinks
  bool schedulable = true;              // no illegitimate deadlock reachable

  std::vector<TaskMetrics> tasks;
  std::vector<LockMetrics> locks;
  std::vector<CoreMetrics> cores;

  bool has_steady_cycle = false;
  size_t recurrent_scc_size = 0;
  long long hyperperiod = 0;  // lcm of task periods (0 if none)
};

// Computes performance metrics from an already-built state-class graph plus the
// lowered net. Task-level metrics rely on net.task_info and the TDG naming
// conventions; a direct .ptpn net (empty task_info) yields structural metrics
// only.
class MetricsAnalyzer {
 public:
  MetricsAnalyzer(const SCGraph& graph, const petri::PTPN& net, SCVertex initial, bool exact);

  [[nodiscard]] MetricsReport analyze();

  static bool save_to_json(const MetricsReport& report, const std::string& file_path);

 private:
  // Flattened view of one task's static topology in the net.
  struct TaskTopology {
    std::string name;
    int core = -1;
    int priority = 0;
    std::vector<size_t> chain_places;       // entry, ready, seg_done, hold, exit
    std::vector<size_t> chain_transitions;  // get_core, exec*, lock*
    std::vector<size_t> exec_transitions;   // timed execution segments
    size_t entry_place = 0;
    size_t end_place = 0;
    bool has_end = false;
  };

  // Flattened, DP-friendly view of one edge. dwell_* use INF_TIME for infinity.
  struct Edge {
    size_t target = 0;
    int transition_id = -1;
    int dwell_min = 0;
    int dwell_max = 0;
  };

  const SCGraph& graph_;
  const petri::PTPN& net_;
  SCVertex initial_;
  bool exact_;

  size_t num_vertices_ = 0;
  std::vector<std::vector<Edge>> out_edges_;  // per vertex
  std::vector<const StateClass*> state_of_;   // per vertex
  std::vector<TaskTopology> tasks_;
  std::vector<int> transition_task_;  // transition -> task index
  std::vector<bool> transition_is_exec_;
  std::vector<std::string> place_lock_;  // place -> lock name ("" none)

  void flatten_graph();
  void build_topology();

  // Predicates over a vertex.
  [[nodiscard]] bool task_in_flight(const TaskTopology& task, size_t v) const;
  [[nodiscard]] bool task_active(const TaskTopology& task, size_t v) const;
  [[nodiscard]] bool task_suspended(const TaskTopology& task, size_t v) const;
  [[nodiscard]] bool core_busy(int core, size_t v) const;
  [[nodiscard]] bool task_blocked_by_lower(const TaskTopology& task, size_t v) const;

  void compute_structural(MetricsReport& report);
  void compute_schedulability(MetricsReport& report);
  void compute_task_timing(MetricsReport& report);
  void compute_locks(MetricsReport& report);
  void compute_utilisation(MetricsReport& report);
};

}  // namespace state_class

#endif  // ANALYSIS_METRICS_METRICS_H
