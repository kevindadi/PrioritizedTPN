#include "analysis/allocation/allocation.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "common/checked_output.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"

namespace allocation {

namespace {

constexpr size_t kParetoLimit = 32;
constexpr double kUtilizationEpsilon = 1e-12;

// One task's static allocation data, filled in task-name order.
struct TaskSpec {
  std::string name;
  std::vector<int> domain;   // sorted ascending, distinct
  long long wcet_upper = 0;  // sum of segment upper bounds
  bool fixed = false;        // domain size is exactly one
};

std::vector<const TaskNode*> collect_task_nodes(const tdg::TDG& graph) {
  std::vector<const TaskNode*> tasks;
  tasks.reserve(graph.all_task.size());
  for (const auto& node : graph.all_task) {
    if (const TaskNode* task = as_task_node(node)) {
      tasks.push_back(task);
    }
  }
  std::sort(tasks.begin(), tasks.end(),
            [](const TaskNode* left, const TaskNode* right) { return left->name < right->name; });
  return tasks;
}

long long wcet_upper_of(const TaskNode& task) {
  long long total = 0;
  for (const auto& segment : task.time) {
    total += segment.second;
  }
  return total;
}

size_t saturating_product(size_t left, size_t right) {
  if (left == 0 || right == 0) {
    return 0;
  }
  if (left > std::numeric_limits<size_t>::max() / right) {
    return std::numeric_limits<size_t>::max();
  }
  return left * right;
}

void set_task_core(tdg::TDG& graph, const std::string& name, int core) {
  for (auto& node : graph.all_task) {
    TaskNode* task = as_task_node(node);
    if (task != nullptr && task->name == name) {
      task->core = core;
      break;
    }
  }
  const auto node_it = graph.nodes_type.find(name);
  if (node_it != graph.nodes_type.end()) {
    TaskNode* task = as_task_node(node_it->second);
    if (task != nullptr) {
      task->core = core;
    }
  }
}

// Compares WCET values with infinity ordered last. Negative means `left` is
// better (smaller / finite).
int compare_time(bool left_infinite, long long left, bool right_infinite, long long right) {
  if (left_infinite != right_infinite) {
    return left_infinite ? 1 : -1;
  }
  if (left_infinite) {
    return 0;
  }
  if (left < right) {
    return -1;
  }
  if (left > right) {
    return 1;
  }
  return 0;
}

int compare_utilization(double left, double right) {
  if (left < right - kUtilizationEpsilon) {
    return -1;
  }
  if (left > right + kUtilizationEpsilon) {
    return 1;
  }
  return 0;
}

int compare_assignments(const std::vector<std::pair<std::string, int>>& left,
                        const std::vector<std::pair<std::string, int>>& right) {
  if (left == right) {
    return 0;
  }
  return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end()) ? -1
                                                                                            : 1;
}

// Total order used to pick `best` and to accept local-search moves.
// Feasible candidates rank first; ties are broken deterministically.
int rank_candidates(const CandidateSummary& left, const CandidateSummary& right,
                    Objective objective) {
  if (left.feasible != right.feasible) {
    return left.feasible ? -1 : 1;
  }

  if (objective == Objective::MAX_WCRT) {
    const int by_time = compare_time(left.max_wcrt_infinite, left.max_wcrt, right.max_wcrt_infinite,
                                     right.max_wcrt);
    if (by_time != 0) {
      return by_time;
    }
  } else if (objective == Objective::MAX_UTILIZATION && left.utilization_complete &&
             right.utilization_complete) {
    const int by_util = compare_utilization(left.max_utilization, right.max_utilization);
    if (by_util != 0) {
      return by_util;
    }
  } else {
    // MAX_LOAD, and the MAX_UTILIZATION fallback when periods are missing.
    if (left.max_core_load != right.max_core_load) {
      return left.max_core_load < right.max_core_load ? -1 : 1;
    }
  }

  if (left.max_core_load != right.max_core_load) {
    return left.max_core_load < right.max_core_load ? -1 : 1;
  }
  const int by_time =
      compare_time(left.max_wcrt_infinite, left.max_wcrt, right.max_wcrt_infinite, right.max_wcrt);
  if (by_time != 0) {
    return by_time;
  }
  return compare_assignments(left.assignment, right.assignment);
}

// True when `left` is at least as good as `right` on all three Pareto metrics
// and strictly better on at least one. Infinity is treated as +infinity. The
// utilization metric is used even when incomplete (see docs/allocation.md).
bool dominates(const CandidateSummary& left, const CandidateSummary& right) {
  const int by_time =
      compare_time(left.max_wcrt_infinite, left.max_wcrt, right.max_wcrt_infinite, right.max_wcrt);
  const bool time_no_worse = by_time <= 0;
  const bool utilization_no_worse = left.max_utilization <= right.max_utilization;
  const bool load_no_worse = left.max_core_load <= right.max_core_load;
  if (!(time_no_worse && utilization_no_worse && load_no_worse)) {
    return false;
  }
  return by_time < 0 || left.max_utilization < right.max_utilization ||
         left.max_core_load < right.max_core_load;
}

bool pareto_sort_less(const CandidateSummary& left, const CandidateSummary& right) {
  const int by_time =
      compare_time(left.max_wcrt_infinite, left.max_wcrt, right.max_wcrt_infinite, right.max_wcrt);
  if (by_time != 0) {
    return by_time < 0;
  }
  if (left.max_core_load != right.max_core_load) {
    return left.max_core_load < right.max_core_load;
  }
  return compare_assignments(left.assignment, right.assignment) < 0;
}

// Builds one candidate's assignment, lowers it, and runs the exact state-class
// analysis. Any lowering/analysis failure yields an infeasible candidate rather
// than aborting the whole search.
CandidateSummary evaluate_candidate(const tdg::TDG& graph, const std::vector<TaskSpec>& specs,
                                    const std::vector<int>& cores, const Options& options,
                                    size_t real_cores, bool utilization_complete) {
  CandidateSummary summary;
  summary.assignment.reserve(specs.size());
  for (size_t i = 0; i < specs.size(); ++i) {
    summary.assignment.emplace_back(specs[i].name, cores[i]);
  }
  summary.utilization_complete = utilization_complete;
  summary.max_wcrt_infinite = false;
  summary.max_wcrt = 0;
  summary.max_utilization = 0.0;
  summary.max_core_load = 0;
  summary.states = 0;
  summary.deadlock_states = 0;

  try {
    tdg::TDG candidate = graph;
    for (size_t i = 0; i < specs.size(); ++i) {
      // Both the lowering input (nodes_type) and the resource/metadata input
      // (all_task) must agree on the chosen core.
      set_task_core(candidate, specs[i].name, cores[i]);
    }

    petri::PTPN ptpn;
    converter::TDG2PN::transform(candidate, ptpn);

    state_class::StateClassReachabilityGraph reachability(ptpn);
    // Fixed exact configuration: equality canonicalization, suspension-clock
    // projection on, no reduction, no extrapolation, no horizon.
    reachability.set_canonicalization_mode(state_class::CanonicalizationMode::EQUALITY);
    reachability.set_suspension_clock_projection(true);
    reachability.set_reduction_mode(state_class::ReductionMode::NONE);
    reachability.set_extrapolation(false);
    reachability.build(options.candidate_max_states);

    const auto& stats = reachability.get_statistics();
    const bool analysis_exact = !stats.truncated;
    state_class::MetricsAnalyzer analyzer(reachability.get_graph(), ptpn,
                                          reachability.get_initial_vertices(), analysis_exact,
                                          /*wcrt_lb_valid=*/true);
    const state_class::MetricsReport report = analyzer.analyze();

    summary.analysis_exact = analysis_exact;
    summary.states = stats.total_states;
    summary.deadlock_states = report.deadlock_states.size();
    summary.feasible = report.deadlock_analysis_certified && report.bounded && !stats.truncated &&
                       !report.truncated && report.schedulable && report.deadlock_states.empty();

    for (const auto& task : report.tasks) {
      if (!task.observed) {
        continue;
      }
      if (task.wcrt.infinite) {
        summary.max_wcrt_infinite = true;
      } else if (!summary.max_wcrt_infinite) {
        summary.max_wcrt = std::max(summary.max_wcrt, task.wcrt.value);
      }
      if (task.has_deadline && task.deadline_missed) {
        summary.deadline_misses.push_back(task.name);
      }
    }
    std::sort(summary.deadline_misses.begin(), summary.deadline_misses.end());

    for (const auto& core : report.cores) {
      if (core.core < 0 || static_cast<size_t>(core.core) >= real_cores) {
        continue;
      }
      summary.max_utilization = std::max(summary.max_utilization, core.util_max);
    }

    std::vector<long long> load_by_core(real_cores, 0);
    for (const auto& task : report.tasks) {
      if (task.core < 0 || static_cast<size_t>(task.core) >= real_cores) {
        continue;
      }
      load_by_core[static_cast<size_t>(task.core)] += task.wcet;
    }
    for (const long long load : load_by_core) {
      summary.max_core_load = std::max(summary.max_core_load, load);
    }
  } catch (const std::exception& error) {
    spdlog::debug("[ALLOC] candidate analysis failed: {}", error.what());
    summary.feasible = false;
    summary.analysis_exact = false;
  }

  return summary;
}

// Deterministic worst-fit-decreasing seed: fixed tasks first, then flexible
// tasks by descending WCET upper bound (ties by name) on the least-loaded
// allowed core (ties by smallest core id).
std::vector<int> seed_assignment(const std::vector<TaskSpec>& specs, size_t real_cores) {
  std::vector<int> cores(specs.size(), 0);
  std::vector<long long> load_by_core(real_cores, 0);

  for (size_t i = 0; i < specs.size(); ++i) {
    if (!specs[i].fixed) {
      continue;
    }
    cores[i] = specs[i].domain.front();
    load_by_core[static_cast<size_t>(cores[i])] += specs[i].wcet_upper;
  }

  std::vector<size_t> flexible;
  for (size_t i = 0; i < specs.size(); ++i) {
    if (!specs[i].fixed) {
      flexible.push_back(i);
    }
  }
  std::sort(flexible.begin(), flexible.end(), [&](size_t left, size_t right) {
    if (specs[left].wcet_upper != specs[right].wcet_upper) {
      return specs[left].wcet_upper > specs[right].wcet_upper;
    }
    return specs[left].name < specs[right].name;
  });

  for (const size_t index : flexible) {
    int best_core = specs[index].domain.front();
    long long best_load = load_by_core[static_cast<size_t>(best_core)];
    for (const int core : specs[index].domain) {
      const long long core_load = load_by_core[static_cast<size_t>(core)];
      if (core_load < best_load) {
        best_core = core;
        best_load = core_load;
      }
    }
    cores[index] = best_core;
    load_by_core[static_cast<size_t>(best_core)] += specs[index].wcet_upper;
  }

  return cores;
}

// Advances a mixed-radix assignment in lexicographic (task-name, core) order.
// The last task varies fastest.
bool advance_assignment(const std::vector<TaskSpec>& specs, std::vector<int>& indices) {
  for (size_t i = specs.size(); i-- > 0;) {
    if (++indices[i] < static_cast<int>(specs[i].domain.size())) {
      return true;
    }
    indices[i] = 0;
  }
  return false;
}

const char* strategy_name(Strategy strategy) {
  return strategy == Strategy::EXHAUSTIVE ? "exhaustive" : "heuristic";
}

const char* objective_name(Objective objective) {
  switch (objective) {
    case Objective::MAX_UTILIZATION:
      return "max-utilization";
    case Objective::MAX_LOAD:
      return "max-load";
    case Objective::MAX_WCRT:
    default:
      return "max-wcrt";
  }
}

nlohmann::json assignment_to_json(const std::vector<std::pair<std::string, int>>& assignment) {
  nlohmann::json array = nlohmann::json::array();
  for (const auto& [task, core] : assignment) {
    array.push_back({{"task", task}, {"core", core}});
  }
  return array;
}

nlohmann::json candidate_to_json(const CandidateSummary& candidate) {
  nlohmann::json json;
  json["assignment"] = assignment_to_json(candidate.assignment);
  json["feasible"] = candidate.feasible;
  json["analysis_exact"] = candidate.analysis_exact;
  json["utilization_complete"] = candidate.utilization_complete;
  json["max_wcrt_infinite"] = candidate.max_wcrt_infinite;
  json["max_wcrt"] =
      candidate.max_wcrt_infinite ? nlohmann::json(nullptr) : nlohmann::json(candidate.max_wcrt);
  json["max_utilization"] = candidate.max_utilization;
  json["max_core_load"] = candidate.max_core_load;
  json["states"] = candidate.states;
  json["deadlock_states"] = candidate.deadlock_states;
  json["deadline_misses"] = candidate.deadline_misses;
  return json;
}

// Incrementally maintains the feasible non-dominated set and the best
// certification-eligible candidate without storing every evaluation.
struct SearchAccumulator {
  size_t feasible_count = 0;
  std::vector<CandidateSummary> pareto;
  std::optional<CandidateSummary> best;
  bool require_certified_wcet = false;
  bool all_wcet_certified = false;
  Objective objective = Objective::MAX_WCRT;

  void add(const CandidateSummary& candidate) {
    if (candidate.feasible) {
      ++feasible_count;

      const bool dominated_by_existing = std::any_of(
          pareto.begin(), pareto.end(),
          [&](const CandidateSummary& existing) { return dominates(existing, candidate); });
      if (!dominated_by_existing) {
        pareto.erase(std::remove_if(pareto.begin(), pareto.end(),
                                    [&](const CandidateSummary& existing) {
                                      return dominates(candidate, existing);
                                    }),
                     pareto.end());
        pareto.push_back(candidate);
      }
    }

    const bool certification_eligible = !require_certified_wcet || all_wcet_certified;
    if (candidate.feasible && certification_eligible &&
        (!best.has_value() || rank_candidates(candidate, best.value(), objective) < 0)) {
      best = candidate;
    }
  }
};

void append_reason(std::string& reason, const std::string& text) {
  if (text.empty()) {
    return;
  }
  if (!reason.empty()) {
    reason += "; ";
  }
  reason += text;
}

}  // namespace

SearchReport search_allocations(const tdg::TDG& graph, const Options& options) {
  SearchReport report;
  report.objective = options.objective;

  const std::vector<const TaskNode*> task_nodes = collect_task_nodes(graph);
  report.all_wcet_certified = true;
  for (const TaskNode* task : task_nodes) {
    report.task_names.push_back(task->name);

    std::string kind = "model";
    const auto provenance_it = graph.task_wcet_provenance.find(task->name);
    if (provenance_it != graph.task_wcet_provenance.end()) {
      kind = provenance_it->second.kind;
    }
    report.wcet_kinds.emplace_back(task->name, kind);
    if (kind != "certified") {
      report.all_wcet_certified = false;
    }
  }

  // Periods drive utilization completeness exactly like lowering does: the
  // last periodic binding for a task wins, absent means aperiodic.
  std::unordered_map<std::string, int> period_of;
  for (const auto& binding : graph.periodic_tasks) {
    period_of[binding.task] = binding.period;
  }
  size_t tasks_without_period = 0;
  for (const TaskNode* task : task_nodes) {
    const auto period_it = period_of.find(task->name);
    if (period_it == period_of.end() || period_it->second <= 0) {
      ++tasks_without_period;
    }
  }
  report.utilization_complete = tasks_without_period == 0;

  if (graph.num_cpus <= 0) {
    report.complete = false;
    report.truncated = false;
    report.reason = "num_cpus must be positive to define real allocation cores";
    return report;
  }
  const size_t real_cores = static_cast<size_t>(graph.num_cpus);

  std::vector<TaskSpec> specs;
  specs.reserve(task_nodes.size());
  for (const TaskNode* task : task_nodes) {
    TaskSpec spec;
    spec.name = task->name;

    const auto domain_it = graph.task_allowed_cores.find(task->name);
    if (domain_it != graph.task_allowed_cores.end()) {
      spec.domain = domain_it->second;
    } else {
      spec.domain = {task->core};
    }
    if (spec.domain.empty()) {
      report.complete = false;
      report.reason = "task '" + task->name + "' has an empty allocation domain";
      return report;
    }

    std::sort(spec.domain.begin(), spec.domain.end());
    spec.domain.erase(std::unique(spec.domain.begin(), spec.domain.end()), spec.domain.end());
    for (const int core : spec.domain) {
      if (core < 0 || static_cast<size_t>(core) >= real_cores) {
        report.complete = false;
        report.reason = "task '" + task->name + "' allows core " + std::to_string(core) +
                        ", but real cores are 0.." + std::to_string(real_cores - 1);
        return report;
      }
    }

    spec.wcet_upper = wcet_upper_of(*task);
    spec.fixed = spec.domain.size() == 1;
    specs.push_back(std::move(spec));
  }

  size_t domain_size = 1;
  for (const TaskSpec& spec : specs) {
    domain_size = saturating_product(domain_size, spec.domain.size());
  }
  report.domain_size = domain_size;

  SearchAccumulator accumulator;
  accumulator.require_certified_wcet = options.require_certified_wcet;
  accumulator.all_wcet_certified = report.all_wcet_certified;
  accumulator.objective = options.objective;

  const bool exhaustive =
      domain_size <= options.max_candidates && domain_size != std::numeric_limits<size_t>::max();

  if (exhaustive) {
    report.strategy = Strategy::EXHAUSTIVE;
    std::vector<int> indices(specs.size(), 0);
    std::vector<int> cores(specs.size(), 0);

    while (true) {
      if (report.evaluated >= options.max_candidates) {
        report.truncated = true;
        break;
      }
      for (size_t i = 0; i < specs.size(); ++i) {
        cores[i] = specs[i].domain[static_cast<size_t>(indices[i])];
      }
      accumulator.add(evaluate_candidate(graph, specs, cores, options, real_cores,
                                         report.utilization_complete));
      ++report.evaluated;
      if (!advance_assignment(specs, indices)) {
        break;
      }
    }
    report.complete = !report.truncated;
  } else {
    report.strategy = Strategy::HEURISTIC;
    report.complete = false;

    if (options.max_candidates == 0) {
      report.truncated = true;
    } else {
      std::vector<int> cores = seed_assignment(specs, real_cores);
      CandidateSummary current =
          evaluate_candidate(graph, specs, cores, options, real_cores, report.utilization_complete);
      ++report.evaluated;
      accumulator.add(current);

      // Flexible tasks are already in task-name order because `specs` is.
      std::vector<size_t> flexible;
      for (size_t i = 0; i < specs.size(); ++i) {
        if (!specs[i].fixed) {
          flexible.push_back(i);
        }
      }

      bool converged = false;
      while (!converged) {
        bool improved = false;
        bool budget_blocked = false;
        for (const size_t task_index : flexible) {
          for (const int core : specs[task_index].domain) {
            if (core == cores[task_index]) {
              continue;
            }
            if (report.evaluated >= options.max_candidates) {
              budget_blocked = true;
              break;
            }
            std::vector<int> trial = cores;
            trial[task_index] = core;
            CandidateSummary candidate = evaluate_candidate(
                graph, specs, trial, options, real_cores, report.utilization_complete);
            ++report.evaluated;
            accumulator.add(candidate);
            if (rank_candidates(candidate, current, options.objective) < 0) {
              current = std::move(candidate);
              cores = std::move(trial);
              improved = true;
              break;
            }
          }
          if (budget_blocked || improved) {
            break;
          }
        }

        if (budget_blocked) {
          report.truncated = true;
          converged = true;
        } else if (!improved) {
          converged = true;
        }
      }

      if (report.truncated) {
        append_reason(report.reason, "candidate evaluation budget (" +
                                         std::to_string(options.max_candidates) +
                                         ") was exhausted before the local search converged");
      }
    }
  }

  report.feasible_count = accumulator.feasible_count;
  report.best = accumulator.best;

  std::sort(accumulator.pareto.begin(), accumulator.pareto.end(), pareto_sort_less);
  if (accumulator.pareto.size() > kParetoLimit) {
    accumulator.pareto.resize(kParetoLimit);
    report.pareto_truncated = true;
  }
  report.pareto = std::move(accumulator.pareto);

  if (options.max_candidates == 0) {
    append_reason(report.reason, "candidate evaluation budget is zero; no candidate was evaluated");
  }

  if (options.objective == Objective::MAX_UTILIZATION && !report.utilization_complete) {
    append_reason(report.reason,
                  "utilization is incomplete (" + std::to_string(tasks_without_period) +
                      " task(s) without a positive period); max-utilization falls back to "
                      "max_core_load");
  }

  if (!report.best.has_value() && options.max_candidates > 0) {
    if (report.feasible_count == 0) {
      append_reason(report.reason, "no feasible allocation among " +
                                       std::to_string(report.evaluated) + " evaluated candidates");
    } else if (options.require_certified_wcet && !report.all_wcet_certified) {
      append_reason(report.reason,
                    "no feasible allocation has fully certified WCET provenance "
                    "(require_certified_wcet=true)");
    } else {
      append_reason(report.reason, "no eligible allocation was found");
    }
  }

  return report;
}

bool save_report_json(const SearchReport& report, const std::string& path) {
  nlohmann::json json;

  nlohmann::json search;
  search["strategy"] = strategy_name(report.strategy);
  search["complete"] = report.complete;
  search["domain_size"] = report.domain_size;
  search["evaluated"] = report.evaluated;
  search["feasible_count"] = report.feasible_count;
  search["truncated"] = report.truncated;
  search["pareto_truncated"] = report.pareto_truncated;
  search["utilization_complete"] = report.utilization_complete;
  search["objective"] = objective_name(report.objective);
  search["task_count"] = report.task_names.size();
  search["reason"] = report.reason;
  json["search"] = std::move(search);

  nlohmann::json kinds = nlohmann::json::array();
  for (const auto& [task, kind] : report.wcet_kinds) {
    kinds.push_back({{"task", task}, {"kind", kind}});
  }
  json["timing_model"] = {
      {"all_certified", report.all_wcet_certified},
      {"kinds", std::move(kinds)},
  };

  if (report.best.has_value()) {
    json["assignment"] = assignment_to_json(report.best->assignment);
    json["best"] = candidate_to_json(report.best.value());
  } else {
    json["assignment"] = nlohmann::json::array();
    json["best"] = nullptr;
  }

  nlohmann::json pareto = nlohmann::json::array();
  for (const CandidateSummary& candidate : report.pareto) {
    pareto.push_back(candidate_to_json(candidate));
  }
  json["pareto"] = std::move(pareto);

  std::ofstream out(path);
  if (!out.is_open()) {
    return false;
  }
  out << json.dump(2) << '\n';
  return ptpn::finish_output(out);
}

}  // namespace allocation
