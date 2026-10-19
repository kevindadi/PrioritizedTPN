#include "app/analysis_diagnostics.h"

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "analysis/metrics/metrics.h"
#include "common/diagnostics.h"

namespace app {

void record_analysis_diagnostics(ptpn::Diagnostics* diagnostics,
                                 const state_class::MetricsReport& report,
                                 bool max_states_truncated, bool horizon_truncated,
                                 size_t max_states) {
  if (diagnostics == nullptr) {
    return;
  }

  const bool deadlock = report.deadlock_analysis_certified && !report.deadlock_states.empty();
  if (deadlock) {
    diagnostics->add("R-ANA-001",
                     "reachable deadlock: " + std::to_string(report.deadlock_states.size()) +
                         " illegitimate sink state(s)",
                     "system:deadlock", std::nullopt,
                     "fix the dependency/lock structure or choose a different policy");
  }

  std::vector<std::string> local_deadlocks;
  nlohmann::json deadline_misses = nlohmann::json::array();
  for (const auto& task : report.tasks) {
    if (report.deadlock_analysis_certified && task.local_deadlock) {
      local_deadlocks.push_back(task.name);
      diagnostics->add("R-ANA-003", "task '" + task.name + "' is in a periodic local deadlock",
                       "task:" + task.name, std::nullopt,
                       "change the task's period, priority, or core assignment so it can complete");
    }
    if (report.wcrt_lb_valid && task.observed && task.has_deadline && !task.wcrt_lb.infinite &&
        task.wcrt_lb.value > task.deadline) {
      deadline_misses.push_back(
          {{"task", task.name}, {"wcrt_lb", task.wcrt_lb.value}, {"deadline", task.deadline}});
      diagnostics->add("R-ANA-002",
                       "task '" + task.name + "' has a certified response-time lower bound " +
                           std::to_string(task.wcrt_lb.value) + " exceeding its deadline " +
                           std::to_string(task.deadline),
                       "task:" + task.name, std::nullopt,
                       "adjust priorities, cores, or periods so the task can meet its deadline");
    }
  }

  const bool counterexample = deadlock || !local_deadlocks.empty() || !deadline_misses.empty();
  const bool truncated = max_states_truncated || horizon_truncated;
  if (truncated) {
    const std::string fix =
        counterexample
            ? "a counterexample was already found; raising -m cannot change the verdict; fix the "
              "model design"
            : "no counterexample found; raising -m may help";
    if (max_states_truncated) {
      diagnostics->add("E-PERF-001",
                       "state-class graph truncated at the max-states bound (" +
                           std::to_string(max_states) + ")",
                       "system:max_states", std::nullopt, fix);
    }
    if (horizon_truncated) {
      diagnostics->add("E-PERF-002",
                       "state-class graph truncated by the time horizon; some expansions were "
                       "pruned",
                       "system:horizon", std::nullopt,
                       counterexample
                           ? "a counterexample was already found; raising -m cannot change the "
                             "verdict; fix the model design"
                           : "no counterexample found; raising -m may help, or increase/disable "
                             "--horizon");
    }
  }

  std::string counterexample_kind;
  if (deadlock) {
    counterexample_kind = "deadlock";
  } else if (!local_deadlocks.empty()) {
    counterexample_kind = "local-deadlock";
  } else if (!deadline_misses.empty()) {
    counterexample_kind = "deadline";
  }

  nlohmann::json analysis = nlohmann::json::object();
  analysis["exact"] = report.exact;
  analysis["truncated"] = truncated;
  analysis["max_states"] = max_states;
  analysis["horizon_truncated"] = horizon_truncated;
  analysis["states"] = report.states;
  analysis["transitions"] = report.transitions;
  analysis["deadlock"] = deadlock;
  analysis["deadlock_states"] = report.deadlock_states.size();
  analysis["schedulable"] = report.deadlock_analysis_certified ? nlohmann::json(report.schedulable)
                                                               : nlohmann::json(nullptr);
  analysis["local_deadlocks"] = local_deadlocks;
  analysis["deadline_misses"] = std::move(deadline_misses);
  analysis["counterexample"] =
      counterexample_kind.empty() ? nlohmann::json(nullptr) : nlohmann::json(counterexample_kind);
  diagnostics->set_result(nlohmann::json{{"analysis", std::move(analysis)}});
}

}  // namespace app
