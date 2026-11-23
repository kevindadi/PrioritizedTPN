#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reductions/cone_projection.h"
#include "cli_test_helpers.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "parse/json.h"
#include "tdg_test_helpers.h"

namespace {

using ptpn_test::load_tdg;

petri::PTPN lower(const std::string& json) {
  tdg::TDG tdg;
  parse::load_tdg_from_string(json, tdg);
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);
  return ptpn;
}

petri::PTPN lower_file(const std::string& path) {
  tdg::TDG tdg = load_tdg(path);
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);
  return ptpn;
}

std::vector<std::string> cone_task_names(const petri::PTPN& net, const std::string& focus) {
  const petri::PTPN cone = petri::project_cone(net, focus);
  std::vector<std::string> names;
  names.reserve(cone.task_info().size());
  for (const auto& [name, info] : cone.task_info()) {
    (void)info;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

const char* kIndependentCores = R"({
  "graph": {"name": "IndependentCores"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "shared_locks": [],
    "policy": "fixed",
    "periodic": [{"task": "A", "period": 10}, {"task": "B", "period": 12}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 2, "core": 0, "time": [[2, 2]], "locks": []},
    {"id": "B", "type": "task", "priority": 1, "core": 1, "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

const char* kSharedLock = R"({
  "graph": {"name": "SharedLock"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "shared_locks": ["mutex0"],
    "policy": "fixed"
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 2, "core": 0,
     "time": [[1, 1], [2, 2], [1, 1]], "locks": ["mutex0"]},
    {"id": "B", "type": "task", "priority": 1, "core": 1,
     "time": [[1, 1], [2, 2], [1, 1]], "locks": ["mutex0"]}
  ],
  "edges": []
})";

const char* kDependencyChain = R"({
  "graph": {"name": "DependencyChain"},
  "configuration": {
    "num_cpus": 3,
    "cores_per_cpu": 1,
    "shared_locks": [],
    "policy": "fixed",
    "periodic": [{"task": "A", "period": 10}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 3, "core": 0, "time": [[2, 2]], "locks": []},
    {"id": "B", "type": "task", "priority": 2, "core": 1, "time": [[2, 2]], "locks": []},
    {"id": "C", "type": "task", "priority": 1, "core": 2, "time": [[2, 2]], "locks": []}
  ],
  "edges": [
    {"source": "A", "target": "B", "label": "1"},
    {"source": "B", "target": "C", "label": "1"}
  ]
})";

const state_class::TaskMetrics* find_task(const state_class::MetricsReport& report,
                                          const std::string& name) {
  const auto it =
      std::find_if(report.tasks.begin(), report.tasks.end(),
                   [&](const state_class::TaskMetrics& metrics) { return metrics.name == name; });
  return it == report.tasks.end() ? nullptr : &*it;
}

void expect_same_task_metrics(const state_class::TaskMetrics& expected,
                              const state_class::TaskMetrics& actual) {
  EXPECT_EQ(actual.name, expected.name);
  EXPECT_EQ(actual.wcrt.value, expected.wcrt.value);
  EXPECT_EQ(actual.wcrt.infinite, expected.wcrt.infinite);
  EXPECT_EQ(actual.wcrt_lb.value, expected.wcrt_lb.value);
  EXPECT_EQ(actual.wcrt_lb.infinite, expected.wcrt_lb.infinite);
  EXPECT_EQ(actual.bcet, expected.bcet);
  EXPECT_EQ(actual.wcet, expected.wcet);
  EXPECT_EQ(actual.deadline_missed, expected.deadline_missed);
  EXPECT_EQ(actual.local_deadlock, expected.local_deadlock);
}

void expect_cone_metrics_match_full_net(const petri::PTPN& full_net, const std::string& label) {
  state_class::StateClassReachabilityGraph full_graph(full_net);
  const size_t full_states = full_graph.build(100000);
  ASSERT_FALSE(full_graph.get_statistics().truncated) << label << " exceeded the test state cap";
  state_class::MetricsAnalyzer full_analyzer(full_graph.get_graph(), full_net,
                                             full_graph.get_initial_vertex(),
                                             /*exact=*/true);
  const state_class::MetricsReport full_report = full_analyzer.analyze();
  ASSERT_GT(full_report.tasks.size(), 0u);
  EXPECT_EQ(full_states, full_graph.get_statistics().total_states);

  for (const auto& expected : full_report.tasks) {
    SCOPED_TRACE(label + " task " + expected.name);
    const petri::PTPN cone_net = petri::project_cone(full_net, expected.name);

    state_class::StateClassReachabilityGraph cone_graph(cone_net);
    cone_graph.build(100000);
    ASSERT_FALSE(cone_graph.get_statistics().truncated)
        << "cone for " << expected.name << " exceeded the test state cap";
    state_class::MetricsAnalyzer cone_analyzer(cone_graph.get_graph(), cone_net,
                                               cone_graph.get_initial_vertex(),
                                               /*exact=*/true);
    const state_class::MetricsReport cone_report = cone_analyzer.analyze();
    const state_class::TaskMetrics* actual = find_task(cone_report, expected.name);
    ASSERT_NE(actual, nullptr) << "cone analysis dropped the focus task";
    expect_same_task_metrics(expected, *actual);
  }
}

void expect_cone_metrics_match_full_graph(const std::string& path) {
  expect_cone_metrics_match_full_net(lower_file(path), path);
}

}  // namespace

TEST(ConeProjection, IndependentCoresExcludeEachOther) {
  const petri::PTPN net = lower(kIndependentCores);
  ASSERT_EQ(net.task_info().count("A"), 1u);
  ASSERT_EQ(net.task_info().count("B"), 1u);

  const std::vector<std::string> cone_a = cone_task_names(net, "A");
  const std::vector<std::string> cone_b = cone_task_names(net, "B");

  // A and B run on different cores, share no lock and have no dependency, so
  // neither cone may pull in the other core's task.
  EXPECT_TRUE(contains(cone_a, "A"));
  EXPECT_FALSE(contains(cone_a, "B"));
  EXPECT_TRUE(contains(cone_b, "B"));
  EXPECT_FALSE(contains(cone_b, "A"));

  // The projected nets must actually be smaller, and the reduced analysis must
  // still reproduce every per-task timing value.
  const petri::PTPN projected_a = petri::project_cone(net, "A");
  EXPECT_LT(projected_a.num_places(), net.num_places());
  EXPECT_LT(projected_a.num_transitions(), net.num_transitions());
  expect_cone_metrics_match_full_net(net, "independent-cores");
}

TEST(ConeProjection, SharedLockPullsBothHoldersIntoEachCone) {
  const petri::PTPN net = lower(kSharedLock);
  ASSERT_EQ(net.task_info().count("A"), 1u);
  ASSERT_EQ(net.task_info().count("B"), 1u);
  ASSERT_EQ(net.task_info().at("A").locks.size(), 1u);
  ASSERT_EQ(net.task_info().at("B").locks.size(), 1u);

  const std::vector<std::string> cone_a = cone_task_names(net, "A");
  const std::vector<std::string> cone_b = cone_task_names(net, "B");

  // The lock holder on the other core must be present: it can block A.
  EXPECT_TRUE(contains(cone_a, "A"));
  EXPECT_TRUE(contains(cone_a, "B"));
  EXPECT_TRUE(contains(cone_b, "A"));
  EXPECT_TRUE(contains(cone_b, "B"));

  const petri::PTPN projected = petri::project_cone(net, "A");
  EXPECT_EQ(projected.lock_places().count("mutex0"), 1u);
}

TEST(ConeProjection, DependencyChainIncludesAllPredecessors) {
  const petri::PTPN net = lower(kDependencyChain);
  for (const char* task : {"A", "B", "C"}) {
    ASSERT_EQ(net.task_info().count(task), 1u) << task;
  }
  ASSERT_EQ(net.task_info().at("A").core, 0);
  ASSERT_EQ(net.task_info().at("B").core, 1);
  ASSERT_EQ(net.task_info().at("C").core, 2);

  const std::vector<std::string> cone_c = cone_task_names(net, "C");

  // C depends on B, which depends on A; every predecessor of the chain belongs
  // to C's cone even though all three tasks run on different cores.
  EXPECT_TRUE(contains(cone_c, "C"));
  EXPECT_TRUE(contains(cone_c, "B"));
  EXPECT_TRUE(contains(cone_c, "A"));

  // C's cone contains exactly the dependency chain; its metrics must still
  // agree with the full graph.
  expect_cone_metrics_match_full_net(net, "dependency-chain");
}

TEST(ConeProjection, PerTaskMetricsMatchFullGraph) {
  for (const char* path : {"test/fixtures/s-bench/a.json", "test/fixtures/s-bench/b.json",
                           "test/fixtures/p-bench/initial.json", "test/fixtures/p-bench/no-period.json"}) {
    SCOPED_TRACE(path);
    expect_cone_metrics_match_full_graph(path);
  }
}

TEST(ConeProjection, UnknownTaskThrows) {
  const petri::PTPN net = lower(kIndependentCores);
  EXPECT_THROW(petri::project_cone(net, "missing"), std::invalid_argument);
}

TEST(PriorityWcrt, DropsLowerPriorityPeriodicInterleavingsAndPreservesResponse) {
  const auto input = ptpn_test::write_temp_file("ptpn", ".json", R"({
    "graph": {"name": "PriorityProjection"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed",
      "start": [{"task": "H"}, {"task": "A"}],
      "periodic": [{"task": "L", "period": 3}]},
    "nodes": [
      {"id": "H", "type": "task", "priority": 3, "time": [[3, 4]], "locks": []},
      {"id": "A", "type": "task", "priority": 2, "deadline": 9,
       "time": [[2, 3]], "locks": []},
      {"id": "L", "type": "task", "priority": 1, "time": [[1, 2]], "locks": []}],
    "edges": []
  })");
  const auto full = ptpn_test::run_cli({"tdg", "-f", input, "--diagnostics", "json"});
  const auto reduced = ptpn_test::run_cli(
      {"tdg", "-f", input, "--priority-wcrt", "A", "--diagnostics", "json"});
  // L misses its own deadline in the full system. The target meets its deadline.
  ASSERT_EQ(full.exit_code, 6) << full.stdout_text << full.stderr_text;
  ASSERT_EQ(reduced.exit_code, 0) << reduced.stdout_text << reduced.stderr_text;
  const auto baseline = nlohmann::json::parse(full.stdout_text).at("result").at("analysis");
  const auto focused = nlohmann::json::parse(reduced.stdout_text).at("result").at("analysis");
  ASSERT_EQ(focused.at("tasks").size(), 1u);
  const auto& task = focused.at("tasks").at(0);
  ASSERT_EQ(task.at("task"), "A");
  EXPECT_EQ(task.at("wcrt"), 7);
  EXPECT_EQ(task.at("verdict"), "meets");
  EXPECT_TRUE(task.at("wcrt_exact").get<bool>());
  EXPECT_EQ(focused.at("scope").at("task"), "A");
  EXPECT_TRUE(focused.at("schedulable").is_null());
  EXPECT_TRUE(focused.at("deadlock").is_null());
  EXPECT_TRUE(focused.at("local_deadlocks").is_null());
  EXPECT_LT(focused.at("states").get<size_t>(), baseline.at("states").get<size_t>());
  bool matched = false;
  for (const auto& row : baseline.at("tasks")) {
    if (row.at("task") == "A") {
      matched = true;
      EXPECT_EQ(row.at("wcrt"), task.at("wcrt"));
      EXPECT_EQ(row.at("verdict"), task.at("verdict"));
    }
  }
  EXPECT_TRUE(matched);
}

TEST(PriorityWcrt, RejectsSharedLocksAndDependencies) {
  for (const char* model : {kSharedLock, kDependencyChain}) {
    const auto input = ptpn_test::write_temp_file("ptpn", ".json", model);
    const auto run = ptpn_test::run_cli(
        {"tdg", "-f", input, "--priority-wcrt", "A", "--diagnostics", "json"});
    ASSERT_EQ(run.exit_code, 4) << run.stdout_text << run.stderr_text;
    EXPECT_NE(run.stdout_text.find("E-UNS-001"), std::string::npos);
  }
}

TEST(PriorityWcrt, KeepsTiedForegroundJobsAndReportsExactDeadlineMiss) {
  const auto input = ptpn_test::write_temp_file("ptpn", ".json", R"({
    "graph": {"name": "PriorityProjectionTies"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fixed",
      "start": [{"task": "H"}, {"task": "A"}, {"task": "B"}],
      "periodic": [{"task": "L", "period": 3}, {"task": "R", "period": 2}]},
    "nodes": [
      {"id": "H", "type": "task", "priority": 3, "time": [[4, 4]], "locks": []},
      {"id": "A", "type": "task", "priority": 2, "deadline": 7,
       "time": [[2, 2]], "locks": []},
      {"id": "B", "type": "task", "priority": 2, "time": [[5, 5]], "locks": []},
      {"id": "L", "type": "task", "priority": 1, "time": [[1, 1]], "locks": []},
      {"id": "R", "type": "task", "core": 1, "priority": 9,
       "time": [[1, 1]], "locks": []}],
    "edges": []
  })");
  const auto metrics = ptpn_test::test_artifact_name("priority", ".json");
  const auto run = ptpn_test::run_cli({"tdg", "-f", input, "--priority-wcrt", "A",
                                      "--diagnostics", "json", "--export-metrics", metrics});
  ASSERT_EQ(run.exit_code, 6) << run.stdout_text << run.stderr_text;
  const auto report = nlohmann::json::parse(ptpn_test::read_file(metrics));
  EXPECT_EQ(report.at("scope").at("task"), "A");
  ASSERT_EQ(report.at("tasks").size(), 1u);
  const auto& task = report.at("tasks").at(0);
  EXPECT_EQ(task.at("wcrt"), 11);
  EXPECT_EQ(task.at("verdict"), "misses");
  EXPECT_TRUE(task.at("wcrt_exact").get<bool>());
  EXPECT_FALSE(task.at("wcrt_capped").get<bool>());
}
