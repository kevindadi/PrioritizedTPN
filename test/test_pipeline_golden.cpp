#include <climits>
#include <cstddef>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/metrics/metrics_json.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "app/pipelines.h"
#include "export/export_dot.h"
#include "export/scg_dot.h"
#include "export/wcet_json.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "parse/json.h"
#include "tdg_test_helpers.h"

namespace {

using ptpn_test::load_tdg;

// Golden outputs are generated with libstdc++ (the stdlib used by CI):
//   ./build/ptpn tdg -f example/motivating-examples/hw.json \
//     --export-ptpn test/golden/hw.ptpn.dot --export-scg test/golden/hw.scg.dot \
//     --export-metrics test/golden/hw.metrics.json
//   ./build/ptpn tdg -f example/s-bench/a.json \
//     --export-ptpn test/golden/a.ptpn.dot --export-metrics test/golden/a.metrics.json

petri::PTPN lower_to_ptpn(const std::string& path) {
  tdg::TDG tdg = load_tdg(path);
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);
  return ptpn;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("cannot read " + path);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void expect_matches_golden(const std::string& actual_path, const std::string& golden_path) {
#ifndef __GLIBCXX__
  GTEST_SKIP() << "golden files are generated with libstdc++";
#endif
  EXPECT_EQ(read_text(actual_path), read_text(golden_path))
      << "regenerate " << golden_path << " if the change is intentional";
}

struct PipelineCase {
  const char* input;
  size_t places;
  size_t transitions;
  size_t states;
  size_t edges;
  size_t dedup_hits;
};

const PipelineCase kPipelineCases[] = {
    {"example/s-bench/a.json", 20, 21, 125, 163, 39},
    {"example/s-bench/b.json", 20, 21, 84, 113, 30},
    {"example/motivating-examples/hw.json", 10, 10, 17, 17, 1},
    {"example/p-bench/initial.json", 20, 21, 80, 106, 27},
    {"example/p-bench/same-priority.json", 20, 21, 94, 123, 30},
    {"example/p-bench/no-period.json", 18, 19, 44, 60, 17},
};

}  // namespace

TEST(PipelineGolden, PlaceTransitionAndStateCounts) {
  for (const auto& test_case : kPipelineCases) {
    SCOPED_TRACE(test_case.input);
    const petri::PTPN ptpn = lower_to_ptpn(test_case.input);
    EXPECT_EQ(ptpn.num_places(), test_case.places);
    EXPECT_EQ(ptpn.num_transitions(), test_case.transitions);

    state_class::StateClassReachabilityGraph graph(ptpn);
    const size_t states = graph.build(5000);
    const auto& stats = graph.get_statistics();
    EXPECT_EQ(states, test_case.states);
    EXPECT_EQ(stats.total_states, test_case.states);
    EXPECT_EQ(stats.total_transitions, test_case.edges);
    EXPECT_EQ(stats.dedup_hits, test_case.dedup_hits);
    EXPECT_FALSE(stats.truncated);
  }
}

TEST(PipelineGolden, TieHeavyModelTruncatesAtCap) {
  // Duplicate priorities on one core are expensive even with the default FIFO
  // tie rule: this model exceeds a 5,000-class budget. The truncated flag must
  // be reported so metrics are not labelled exact.
  const petri::PTPN ptpn = lower_to_ptpn("example/p-bench/same-core.json");
  state_class::StateClassReachabilityGraph graph(ptpn);
  const size_t states = graph.build(5000);
  const auto& stats = graph.get_statistics();
  EXPECT_EQ(states, 5000u);
  EXPECT_TRUE(stats.truncated);
}

TEST(PipelineGolden, PtpnDotMatchesGolden) {
  for (const auto& test_case :
       {std::make_pair("example/motivating-examples/hw.json", "test/golden/hw.ptpn.dot"),
        std::make_pair("example/s-bench/a.json", "test/golden/a.ptpn.dot")}) {
    SCOPED_TRACE(test_case.first);
    const petri::PTPN ptpn = lower_to_ptpn(test_case.first);
    const auto model = petri::exporting::build_export_model(ptpn);
    const std::string out = testing::TempDir() + "ptpn_test_ptpn.dot";
    ASSERT_TRUE(petri::exporting::save_to_dot(model, out));
    expect_matches_golden(out, test_case.second);
  }
}

TEST(PipelineGolden, ScgDotMatchesGolden) {
  const petri::PTPN ptpn = lower_to_ptpn("example/motivating-examples/hw.json");
  state_class::StateClassReachabilityGraph graph(ptpn);
  graph.build(5000);
  const std::string out = testing::TempDir() + "ptpn_test_scg.dot";
  const scg_export::ScgFormatter formatter(ptpn, graph.get_graph());
  ASSERT_TRUE(formatter.save_to_dot(out));
  expect_matches_golden(out, "test/golden/hw.scg.dot");
}

TEST(PipelineGolden, MetricsJsonMatchesGolden) {
  for (const auto& test_case :
       {std::make_pair("example/motivating-examples/hw.json", "test/golden/hw.metrics.json"),
        std::make_pair("example/s-bench/a.json", "test/golden/a.metrics.json")}) {
    SCOPED_TRACE(test_case.first);
    const petri::PTPN ptpn = lower_to_ptpn(test_case.first);
    state_class::StateClassReachabilityGraph graph(ptpn);
    graph.build(5000);
    state_class::MetricsAnalyzer analyzer(graph.get_graph(), ptpn, graph.get_initial_vertex(),
                                          /*exact=*/true);
    const state_class::MetricsReport report = analyzer.analyze();
    const std::string out = testing::TempDir() + "ptpn_test_metrics.json";
    ASSERT_TRUE(state_class::save_metrics_json(report, out));
    expect_matches_golden(out, test_case.second);
  }
}

TEST(PipelineGolden, WcetJsonSumsLargeIntervalsWithoutOverflow) {
  tdg::TDG tdg(1, 1);
  TaskNode big;
  big.name = "Big";
  big.time = {{INT_MAX, INT_MAX}, {INT_MAX, INT_MAX}};
  tdg.all_task.emplace_back(big);

  const std::string out = testing::TempDir() + "ptpn_test_wcet_overflow.json";
  ASSERT_TRUE(exporting::write_wcet_json(tdg, out));

  nlohmann::json exported = nlohmann::json::parse(read_text(out), nullptr, false);
  ASSERT_FALSE(exported.is_discarded());
  ASSERT_EQ(exported["tasks"].size(), 1);
  EXPECT_EQ(exported["tasks"][0]["wcet"].get<long long>(), 2LL * INT_MAX);
  EXPECT_EQ(exported["tasks"][0]["interval"],
            nlohmann::json::array({2LL * INT_MAX, 2LL * INT_MAX}));
}

TEST(PipelineGolden, WcetJsonReportsIntervalsProvenanceAndCertification) {
  tdg::TDG tdg(1, 2);

  TaskNode certified;
  certified.name = "Certified";
  certified.time = {{1, 2}, {3, 5}};
  tdg.all_task.emplace_back(certified);
  tdg.task_wcet_provenance["Certified"] = {"certified", "OTAWA", "sha256:abc"};

  // No provenance entry: the export must fall back to kind "model".
  TaskNode modeled;
  modeled.name = "Modeled";
  modeled.time = {{0, 7}};
  tdg.all_task.emplace_back(modeled);

  const std::string out = testing::TempDir() + "ptpn_test_wcet.json";
  ASSERT_TRUE(exporting::write_wcet_json(tdg, out));

  nlohmann::json exported;
  {
    std::ifstream in(out);
    ASSERT_TRUE(in.is_open());
    in >> exported;
  }

  EXPECT_FALSE(exported["all_certified"].get<bool>());
  ASSERT_EQ(exported["tasks"].size(), 2);

  const auto& certified_json = exported["tasks"][0];
  EXPECT_EQ(certified_json["id"], 0);
  EXPECT_EQ(certified_json["name"], "Certified");
  EXPECT_EQ(certified_json["wcet"], 7);
  EXPECT_EQ(certified_json["interval"], nlohmann::json::array({4, 7}));
  EXPECT_EQ(certified_json["segments"], 2);
  EXPECT_EQ(certified_json["wcet_kind"], "certified");
  EXPECT_EQ(certified_json["wcet_method"], "OTAWA");
  EXPECT_EQ(certified_json["wcet_artifact"], "sha256:abc");

  const auto& modeled_json = exported["tasks"][1];
  EXPECT_EQ(modeled_json["name"], "Modeled");
  EXPECT_EQ(modeled_json["wcet"], 7);
  EXPECT_EQ(modeled_json["interval"], nlohmann::json::array({0, 7}));
  EXPECT_EQ(modeled_json["wcet_kind"], "model");
  EXPECT_EQ(modeled_json["wcet_method"], "");
  EXPECT_EQ(modeled_json["wcet_artifact"], "");

  // All tasks certified is the only case that sets the top-level flag.
  tdg.task_wcet_provenance["Modeled"] = {"certified", "OTAWA", "sha256:def"};
  const std::string all_certified_path = testing::TempDir() + "ptpn_test_wcet_certified.json";
  ASSERT_TRUE(exporting::write_wcet_json(tdg, all_certified_path));
  std::ifstream in(all_certified_path);
  ASSERT_TRUE(in.is_open());
  nlohmann::json all_certified;
  in >> all_certified;
  EXPECT_TRUE(all_certified["all_certified"].get<bool>());
}
