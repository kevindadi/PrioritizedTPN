// Reference-analysis cross-checks for the exact WCRT observation:
//
//  * classic fixed-priority response-time analysis (RTA) with the critical
//    instant, and
//  * an independent tick-based discrete-time simulator.
//
// Both are computed from the same task parameters as the analyzer input, so a
// mismatch points at the state-class analysis (or at the observation) rather
// than at a shared helper. Only distinct priorities are used: with equal
// priorities the analyzer explores every tie-break, which no single simulator
// run can reproduce.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "parse/json.h"

namespace {

struct TaskSpec {
  std::string name;
  int period = 0;
  int wcet = 0;
  int priority = 0;
};

// A single-core fixed-priority periodic task set, all released synchronously at
// time 0 (the critical instant), deterministic execution [wcet, wcet].
std::string make_tdg_json(const std::vector<TaskSpec>& tasks) {
  std::string json =
      R"({"graph": {"name": "reference"}, "configuration": {"num_cpus": 1, )"
      R"("cores_per_cpu": 1, "policy": "fixed", "start": [)";
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (i) {
      json += ", ";
    }
    json += R"({"task": ")" + tasks[i].name + R"(", "tokens": 1})";
  }
  json += R"(], "periodic": [)";
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (i) {
      json += ", ";
    }
    json += R"({"task": ")" + tasks[i].name + R"(", "period": )" +
            std::to_string(tasks[i].period) + "}";
  }
  json += R"(]}, "nodes": [)";
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (i) {
      json += ", ";
    }
    json += R"({"id": ")" + tasks[i].name + R"(", "type": "task", "priority": )" +
            std::to_string(tasks[i].priority) + R"(, "core": 0, "deadline": )" +
            std::to_string(tasks[i].period) + R"(, "time": [[)" +
            std::to_string(tasks[i].wcet) + ", " + std::to_string(tasks[i].wcet) +
            R"(]], "locks": []})";
  }
  json += R"(], "edges": []})";
  return json;
}

state_class::MetricsReport analyze(const std::vector<TaskSpec>& tasks) {
  tdg::TDG tdg;
  parse::load_tdg_from_string(make_tdg_json(tasks), tdg);
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);
  state_class::StateClassReachabilityGraph graph(ptpn);
  graph.set_completion_observation(true);
  // The generated models stay small (a handful of tasks, hyperperiod <= 4000);
  // the cap is a safety net so a regression cannot eat the machine.
  graph.build(50000);
  EXPECT_FALSE(graph.get_statistics().truncated);
  state_class::MetricsAnalyzer analyzer(graph.get_graph(), ptpn, graph.get_initial_vertices(),
                                        /*exact=*/true);
  analyzer.set_completion_observation(graph.observation_active());
  return analyzer.analyze();
}

const state_class::TaskMetrics* task_metrics(const state_class::MetricsReport& report,
                                             const std::string& name) {
  const auto it =
      std::find_if(report.tasks.begin(), report.tasks.end(),
                   [&](const state_class::TaskMetrics& task) { return task.name == name; });
  return it == report.tasks.end() ? nullptr : &*it;
}

// Classic RTA: R_i = C_i + sum over higher-priority tasks of
// ceil(R_i / T_j) * C_j, starting from R_i = C_i. Returns the fixed point, or
// -1 when the iteration exceeds the task's period (unschedulable).
long long classic_rta(const std::vector<TaskSpec>& tasks, std::size_t index) {
  const TaskSpec& task = tasks[index];
  long long response = task.wcet;
  for (;;) {
    long long interference = 0;
    for (std::size_t j = 0; j < tasks.size(); ++j) {
      if (j == index || tasks[j].priority <= task.priority) {
        continue;
      }
      interference += ((response + tasks[j].period - 1) / tasks[j].period) * tasks[j].wcet;
    }
    const long long next = task.wcet + interference;
    if (next == response) {
      return response;
    }
    response = next;
    if (response > task.period) {
      return -1;  // the classic iteration diverges
    }
  }
}

// Tick-based simulator with the same capacity-1 merge semantics as the
// lowering: a release that fires while the job is still in flight merges into
// the pending job instead of queueing.
struct SimulationResult {
  std::vector<long long> max_response;
  std::vector<int> completions;
};

SimulationResult simulate(const std::vector<TaskSpec>& tasks, long long horizon) {
  const std::size_t n = tasks.size();
  SimulationResult result;
  result.max_response.assign(n, 0);
  result.completions.assign(n, 0);

  struct Job {
    bool in_flight = false;
    long long release = 0;
    long long remaining = 0;
  };
  std::vector<Job> jobs(n);
  std::vector<long long> next_release(n);
  for (std::size_t i = 0; i < n; ++i) {
    next_release[i] = 0;
  }

  for (long long t = 0; t < horizon; ++t) {
    for (std::size_t i = 0; i < n; ++i) {
      if (next_release[i] == t) {
        next_release[i] += tasks[i].period;
        if (!jobs[i].in_flight) {
          jobs[i].in_flight = true;
          jobs[i].release = t;
          jobs[i].remaining = tasks[i].wcet;
        }
        // else: the release merges into the pending job (no clock reset).
      }
    }

    // Highest-priority ready task runs for one tick.
    std::size_t chosen = n;
    for (std::size_t i = 0; i < n; ++i) {
      if (!jobs[i].in_flight) {
        continue;
      }
      if (chosen == n || tasks[i].priority > tasks[chosen].priority) {
        chosen = i;
      }
    }
    if (chosen == n) {
      continue;  // idle
    }
    if (--jobs[chosen].remaining == 0) {
      result.max_response[chosen] =
          std::max(result.max_response[chosen], t + 1 - jobs[chosen].release);
      result.completions[chosen]++;
      jobs[chosen].in_flight = false;
    }
  }
  return result;
}

long long hyperperiod(const std::vector<TaskSpec>& tasks) {
  long long h = 1;
  for (const TaskSpec& task : tasks) {
    h = std::lcm(h, static_cast<long long>(task.period));
  }
  return h;
}

// Deterministic pseudo-random task sets with distinct priorities.
std::vector<TaskSpec> random_task_set(std::mt19937& rng, int count) {
  std::uniform_int_distribution<int> period_dist(3, 24);
  std::vector<TaskSpec> tasks;
  for (int i = 0; i < count; ++i) {
    TaskSpec task;
    task.name = "T" + std::to_string(i);
    task.period = period_dist(rng);
    std::uniform_int_distribution<int> wcet_dist(1, std::max(1, task.period / 3));
    task.wcet = wcet_dist(rng);
    tasks.push_back(task);
  }
  // Distinct priorities: shorter periods get higher priorities (RM), with the
  // highest priority numbered last.
  std::sort(tasks.begin(), tasks.end(),
            [](const TaskSpec& a, const TaskSpec& b) { return a.period < b.period; });
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    tasks[i].priority = static_cast<int>(tasks.size() - i);
  }
  return tasks;
}

bool all_schedulable(const std::vector<TaskSpec>& tasks) {
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (classic_rta(tasks, i) < 0) {
      return false;
    }
  }
  return true;
}

const std::vector<TaskSpec>& fixed_task_set() {
  // Rate-monotonic and schedulable: R_C = 1, R_B = 3, R_A = 5.
  static const std::vector<TaskSpec> tasks = {
      {"A", 20, 2, 1},  // lowest priority
      {"B", 10, 2, 2},
      {"C", 5, 1, 3},  // highest priority
  };
  return tasks;
}

TEST(ReferenceAnalysisTest, FixedTaskSetMatchesClassicRtaAndSimulator) {
  const std::vector<TaskSpec>& tasks = fixed_task_set();
  const state_class::MetricsReport report = analyze(tasks);
  ASSERT_EQ(report.tasks.size(), tasks.size());
  EXPECT_TRUE(report.wcrt_exact);
  EXPECT_EQ(report.schedulability, state_class::Schedulability::Schedulable);

  const SimulationResult simulation = simulate(tasks, hyperperiod(tasks));
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    const long long rta = classic_rta(tasks, i);
    ASSERT_GE(rta, 0) << tasks[i].name;
    const state_class::TaskMetrics* metrics = task_metrics(report, tasks[i].name);
    ASSERT_NE(metrics, nullptr);
    EXPECT_TRUE(metrics->wcrt_exact) << tasks[i].name;
    EXPECT_EQ(metrics->wcrt.value, rta) << tasks[i].name;
    EXPECT_EQ(metrics->wcrt_lb.value, rta) << tasks[i].name;
    EXPECT_EQ(simulation.max_response[i], rta) << tasks[i].name;
    EXPECT_EQ(metrics->verdict, state_class::DeadlineVerdict::Meets) << tasks[i].name;
  }
}

TEST(ReferenceAnalysisTest, RandomSchedulableTaskSetsMatchClassicRta) {
  std::mt19937 rng(20261020);
  int checked = 0;
  for (int round = 0; round < 200 && checked < 60; ++round) {
    const std::vector<TaskSpec> tasks = random_task_set(rng, 2 + (round % 3));
    if (!all_schedulable(tasks)) {
      continue;  // the classic iteration diverges; the merge semantics differ
    }
    if (hyperperiod(tasks) > 4000) {
      continue;  // keep the state-class exploration small
    }
    ++checked;
    const state_class::MetricsReport report = analyze(tasks);
    ASSERT_EQ(report.schedulability, state_class::Schedulability::Schedulable)
        << make_tdg_json(tasks);
    const SimulationResult simulation = simulate(tasks, hyperperiod(tasks));
    for (std::size_t i = 0; i < tasks.size(); ++i) {
      const long long rta = classic_rta(tasks, i);
      const state_class::TaskMetrics* metrics = task_metrics(report, tasks[i].name);
      ASSERT_NE(metrics, nullptr);
      EXPECT_EQ(metrics->wcrt.value, rta) << tasks[i].name << " of " << make_tdg_json(tasks);
      EXPECT_EQ(metrics->wcrt_lb.value, rta) << tasks[i].name;
      EXPECT_EQ(simulation.max_response[i], rta) << tasks[i].name;
      EXPECT_TRUE(metrics->wcrt_exact) << tasks[i].name;
    }
  }
  EXPECT_GT(checked, 0);
}

}  // namespace
