#ifndef ANALYSIS_ALLOCATION_ALLOCATION_H
#define ANALYSIS_ALLOCATION_ALLOCATION_H

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/tdg.h"

namespace allocation {

// Objective minimized over the evaluated candidates. MAX_UTILIZATION falls
// back to MAX_LOAD when some task has no period (and therefore no analytic
// utilization); see SearchReport::utilization_complete.
enum class Objective { MAX_WCRT, MAX_UTILIZATION, MAX_LOAD };

// EXHAUSTIVE enumerates the full assignment domain when it fits in the
// candidate budget. HEURISTIC seeds a worst-fit-decreasing assignment and
// improves it with deterministic first-improvement local search.
enum class Strategy { EXHAUSTIVE, HEURISTIC };

struct Options {
  // Upper bound on the number of PTPN analyses. Also the exhaustive/heuristic
  // switch: a domain of at most this many assignments is enumerated fully.
  size_t max_candidates = 64;
  // Per-candidate state-class graph bound. Reaching it makes the candidate
  // inexact and infeasible, but the search still completes deterministically.
  size_t candidate_max_states = 200000;
  Objective objective = Objective::MAX_WCRT;
  // When true, only candidates whose every task has "certified" WCET
  // provenance are eligible for `best`.
  bool require_certified_wcet = false;
};

// One evaluated assignment and the metrics extracted from its exact PTPN
// analysis. `assignment` is ordered by task name and lists the core chosen for
// each task.
struct CandidateSummary {
  std::vector<std::pair<std::string, int>> assignment;
  bool feasible = false;
  // True when the state-class graph was built without truncation (equality
  // canonicalization, no extrapolation/reduction/horizon are fixed).
  bool analysis_exact = false;
  // True when every task has period > 0, so max_utilization is a complete
  // analytic objective.
  bool utilization_complete = false;
  bool max_wcrt_infinite = false;
  // Valid only when max_wcrt_infinite is false. Unobserved tasks count as 0.
  long long max_wcrt = 0;
  double max_utilization = 0.0;
  long long max_core_load = 0;
  size_t states = 0;
  size_t deadlock_states = 0;
  // Task names with has_deadline && deadline_missed, sorted.
  std::vector<std::string> deadline_misses;
};

struct SearchReport {
  Strategy strategy = Strategy::EXHAUSTIVE;
  // True only when the assignment domain was exhausted. This says nothing
  // about whether each candidate's PTPN analysis was truncated; consult
  // CandidateSummary::analysis_exact for that.
  bool complete = false;
  // Product of the per-task domain sizes, saturated at SIZE_MAX.
  size_t domain_size = 0;
  size_t evaluated = 0;
  size_t feasible_count = 0;
  // The candidate evaluation budget stopped the search early.
  bool truncated = false;
  bool all_wcet_certified = false;
  // Task names in the deterministic search order (sorted by name).
  std::vector<std::string> task_names;
  // Feasible, non-dominated candidates (at most 32 entries).
  std::vector<CandidateSummary> pareto;
  // Best feasible, certification-eligible candidate (nullopt when none).
  std::optional<CandidateSummary> best;
  std::string reason;

  // Report metadata used by the JSON export.
  Objective objective = Objective::MAX_WCRT;
  bool utilization_complete = true;
  // True when the Pareto list was capped at 32 entries.
  bool pareto_truncated = false;
  // (task name, WCET kind) in task-name order.
  std::vector<std::pair<std::string, std::string>> wcet_kinds;
};

// Evaluates candidate core assignments with the exact PTPN analysis and returns
// a deterministic feasible/Pareto report. Domains come from
// `graph.task_allowed_cores` with a `{task.core}` fallback; cores must be in
// `0 .. graph.num_cpus - 1`.
SearchReport search_allocations(const tdg::TDG& graph, const Options& options);

// Writes the report as deterministic JSON. Returns false when the file cannot
// be opened, written, flushed or closed.
bool save_report_json(const SearchReport& report, const std::string& path);

}  // namespace allocation

#endif  // ANALYSIS_ALLOCATION_ALLOCATION_H
