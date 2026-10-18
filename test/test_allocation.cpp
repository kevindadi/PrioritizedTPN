#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "analysis/allocation/allocation.h"
#include "model/tdg.h"

namespace {

tdg::TDG parse_tdg(const std::string& json) {
  tdg::TDG graph;
  graph.parse_json_string(json);
  return graph;
}

using Assignment = std::vector<std::pair<std::string, int>>;

// A is flexible on {0,1} and has priority 2; B is fixed to core 0 and has
// priority 1. With A on core 0, B waits for A and its response time is 8; with
// A on core 1 both run independently and the maximum WCRT is 5.
const char* kExhaustiveGraph = R"({
  "graph": {"name": "Exhaustive"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "fixed",
    "start": [{"task": "A", "tokens": 1}, {"task": "B", "tokens": 1}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 2, "core": 0,
     "allowed_cores": [0, 1], "time": [[5, 5]], "locks": []},
    {"id": "B", "type": "task", "priority": 1, "core": 0,
     "time": [[3, 3]], "locks": []}
  ],
  "edges": []
})";

// Same as kExhaustiveGraph, but B's deadline is 5. Placing A on core 0 gives B
// a WCRT of 8 and misses the deadline; only A on core 1 is feasible.
const char* kDeadlineGraph = R"({
  "graph": {"name": "Deadline"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "fixed",
    "start": [{"task": "A", "tokens": 1}, {"task": "B", "tokens": 1}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 2, "core": 0,
     "allowed_cores": [0, 1], "time": [[5, 5]], "locks": []},
    {"id": "B", "type": "task", "priority": 1, "core": 0, "deadline": 5,
     "time": [[3, 3]], "locks": []}
  ],
  "edges": []
})";

const char* kFixedGraph = R"({
  "graph": {"name": "Fixed"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "fixed",
    "start": [{"task": "A", "tokens": 1}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 1, "core": 0,
     "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

const char* kHeuristicGraph = R"({
  "graph": {"name": "Heuristic"},
  "configuration": {
    "num_cpus": 3,
    "cores_per_cpu": 1,
    "policy": "fixed",
    "start": [
      {"task": "T0", "tokens": 1},
      {"task": "T1", "tokens": 1},
      {"task": "T2", "tokens": 1}
    ]
  },
  "nodes": [
    {"id": "T0", "type": "task", "priority": 3, "core": 0,
     "allowed_cores": [0, 1, 2], "time": [[2, 2]], "locks": []},
    {"id": "T1", "type": "task", "priority": 2, "core": 1,
     "allowed_cores": [0, 1, 2], "time": [[3, 3]], "locks": []},
    {"id": "T2", "type": "task", "priority": 1, "core": 2,
     "allowed_cores": [0, 1, 2], "time": [[4, 4]], "locks": []}
  ],
  "edges": []
})";

// Execution time 5 with period/deadline 2: the analyzer reports a deadline
// miss, so no allocation can be feasible.
const char* kInfeasibleGraph = R"({
  "graph": {"name": "Infeasible"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "fixed",
    "periodic": [{"task": "A", "period": 2}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 1, "core": 0,
     "time": [[5, 5]], "locks": []}
  ],
  "edges": []
})";

std::string single_task_graph(const std::string& wcet_kind) {
  return R"({
    "graph": {"name": "Provenance"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "fixed",
      "start": [{"task": "A", "tokens": 1}]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0,
       "wcet_kind": ")" +
         wcet_kind + R"(", "time": [[2, 2]], "locks": []}
    ],
    "edges": []
  })";
}

}  // namespace

TEST(AllocationTest, DeadlinePropagationExcludesInfeasibleAssignments) {
  const tdg::TDG graph = parse_tdg(kDeadlineGraph);
  allocation::Options options;
  options.objective = allocation::Objective::MAX_WCRT;

  const allocation::SearchReport report = allocation::search_allocations(graph, options);

  EXPECT_EQ(report.strategy, allocation::Strategy::EXHAUSTIVE);
  EXPECT_TRUE(report.complete);
  EXPECT_EQ(report.evaluated, 2u);
  ASSERT_TRUE(report.best.has_value());
  EXPECT_EQ(report.best->assignment, (Assignment{{"A", 1}, {"B", 0}}));
  EXPECT_EQ(report.best->deadline_misses.size(), 0u);
}

TEST(AllocationTest, ExhaustiveEnumeratesAndMinimizesMaxWcrt) {
  const tdg::TDG graph = parse_tdg(kExhaustiveGraph);
  allocation::Options options;
  options.objective = allocation::Objective::MAX_WCRT;

  const allocation::SearchReport report = allocation::search_allocations(graph, options);

  EXPECT_EQ(report.strategy, allocation::Strategy::EXHAUSTIVE);
  EXPECT_TRUE(report.complete);
  EXPECT_FALSE(report.truncated);
  EXPECT_EQ(report.domain_size, 2u);
  EXPECT_EQ(report.evaluated, 2u);
  EXPECT_EQ(report.feasible_count, 2u);
  EXPECT_EQ(report.task_names, (std::vector<std::string>{"A", "B"}));

  ASSERT_TRUE(report.best.has_value());
  EXPECT_TRUE(report.best->feasible);
  EXPECT_TRUE(report.best->analysis_exact);
  EXPECT_EQ(report.best->assignment, (Assignment{{"A", 1}, {"B", 0}}));
  EXPECT_FALSE(report.best->max_wcrt_infinite);
  EXPECT_EQ(report.best->max_wcrt, 5);
  EXPECT_EQ(report.best->max_core_load, 5);
  EXPECT_TRUE(report.best->deadline_misses.empty());
  EXPECT_FALSE(report.pareto.empty());
}

TEST(AllocationTest, FixedDomainsAreEvaluatedOnce) {
  const tdg::TDG graph = parse_tdg(kFixedGraph);
  allocation::Options options;

  const allocation::SearchReport report = allocation::search_allocations(graph, options);

  EXPECT_EQ(report.strategy, allocation::Strategy::EXHAUSTIVE);
  EXPECT_TRUE(report.complete);
  EXPECT_EQ(report.domain_size, 1u);
  EXPECT_EQ(report.evaluated, 1u);
  EXPECT_EQ(report.feasible_count, 1u);

  ASSERT_TRUE(report.best.has_value());
  EXPECT_EQ(report.best->assignment, (Assignment{{"A", 0}}));
  EXPECT_TRUE(report.best->feasible);
  EXPECT_FALSE(report.best->max_wcrt_infinite);
  EXPECT_EQ(report.best->max_wcrt, 2);
  EXPECT_EQ(report.best->max_core_load, 2);
}

TEST(AllocationTest, HeuristicRespectsCandidateBudget) {
  const tdg::TDG graph = parse_tdg(kHeuristicGraph);
  allocation::Options options;
  options.max_candidates = 4;

  const allocation::SearchReport report = allocation::search_allocations(graph, options);

  EXPECT_EQ(report.strategy, allocation::Strategy::HEURISTIC);
  EXPECT_FALSE(report.complete);
  EXPECT_EQ(report.domain_size, 27u);
  EXPECT_GE(report.evaluated, 1u);
  EXPECT_LE(report.evaluated, options.max_candidates);
}

TEST(AllocationTest, DeadlineMissMakesAllCandidatesInfeasible) {
  const tdg::TDG graph = parse_tdg(kInfeasibleGraph);
  allocation::Options options;

  const allocation::SearchReport report = allocation::search_allocations(graph, options);

  EXPECT_EQ(report.strategy, allocation::Strategy::EXHAUSTIVE);
  EXPECT_EQ(report.domain_size, 1u);
  EXPECT_EQ(report.evaluated, 1u);
  EXPECT_EQ(report.feasible_count, 0u);
  EXPECT_FALSE(report.best.has_value());
  EXPECT_TRUE(report.pareto.empty());
  EXPECT_NE(report.reason.find("no feasible allocation"), std::string::npos);
}

TEST(AllocationTest, CertificationGateRequiresCertifiedProvenance) {
  allocation::Options options;
  options.require_certified_wcet = true;

  const tdg::TDG measured = parse_tdg(single_task_graph("measured"));
  const allocation::SearchReport measured_report =
      allocation::search_allocations(measured, options);

  EXPECT_FALSE(measured_report.all_wcet_certified);
  EXPECT_EQ(measured_report.feasible_count, 1u);
  EXPECT_FALSE(measured_report.best.has_value());
  EXPECT_NE(measured_report.reason.find("certified"), std::string::npos);
  ASSERT_EQ(measured_report.wcet_kinds.size(), 1u);
  EXPECT_EQ(measured_report.wcet_kinds.front().second, "measured");

  const tdg::TDG certified = parse_tdg(single_task_graph("certified"));
  const allocation::SearchReport certified_report =
      allocation::search_allocations(certified, options);

  EXPECT_TRUE(certified_report.all_wcet_certified);
  ASSERT_TRUE(certified_report.best.has_value());
  EXPECT_TRUE(certified_report.best->feasible);
}

TEST(AllocationTest, ReportJsonHasStableSchema) {
  const tdg::TDG graph = parse_tdg(kExhaustiveGraph);
  allocation::Options options;
  const allocation::SearchReport report = allocation::search_allocations(graph, options);

  const std::string path = ::testing::TempDir() + "ptpn_allocation_report.json";
  ASSERT_TRUE(allocation::save_report_json(report, path));

  std::ifstream in(path);
  ASSERT_TRUE(in.is_open());
  nlohmann::json json;
  in >> json;

  ASSERT_TRUE(json.contains("search"));
  ASSERT_TRUE(json.contains("timing_model"));
  ASSERT_TRUE(json.contains("assignment"));
  ASSERT_TRUE(json.contains("best"));
  ASSERT_TRUE(json.contains("pareto"));

  EXPECT_EQ(json["search"]["strategy"], "exhaustive");
  EXPECT_EQ(json["search"]["objective"], "max-wcrt");
  EXPECT_EQ(json["search"]["domain_size"], 2);
  EXPECT_EQ(json["search"]["evaluated"], 2);
  EXPECT_EQ(json["search"]["complete"], true);
  EXPECT_EQ(json["search"]["truncated"], false);

  ASSERT_TRUE(json["timing_model"].contains("all_certified"));
  EXPECT_EQ(json["timing_model"]["all_certified"], false);
  ASSERT_TRUE(json["timing_model"]["kinds"].is_array());
  ASSERT_EQ(json["timing_model"]["kinds"].size(), 2u);
  EXPECT_EQ(json["timing_model"]["kinds"][0]["task"], "A");
  EXPECT_EQ(json["timing_model"]["kinds"][0]["kind"], "model");

  ASSERT_TRUE(json["assignment"].is_array());
  ASSERT_EQ(json["assignment"].size(), 2u);
  EXPECT_EQ(json["assignment"][0]["task"], "A");
  EXPECT_EQ(json["assignment"][0]["core"], 1);
  EXPECT_EQ(json["assignment"][1]["task"], "B");
  EXPECT_EQ(json["assignment"][1]["core"], 0);

  ASSERT_TRUE(json["best"].is_object());
  ASSERT_TRUE(json["best"]["assignment"].is_array());
  EXPECT_EQ(json["best"]["feasible"], true);
  EXPECT_EQ(json["best"]["max_wcrt"], 5);
  EXPECT_EQ(json["best"]["max_wcrt_infinite"], false);
  EXPECT_EQ(json["best"]["max_core_load"], 5);
  ASSERT_TRUE(json["best"]["deadline_misses"].is_array());

  ASSERT_TRUE(json["pareto"].is_array());
  EXPECT_FALSE(json["pareto"].empty());
  EXPECT_TRUE(json["pareto"][0].contains("max_utilization"));
}

TEST(AllocationTest, ReportWriteFailsForMissingDirectory) {
  const tdg::TDG graph = parse_tdg(kFixedGraph);
  allocation::Options options;
  const allocation::SearchReport report = allocation::search_allocations(graph, options);

  EXPECT_FALSE(
      allocation::save_report_json(report, "/nonexistent-ptpn-allocation-dir/report.json"));
}
