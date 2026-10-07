#include <cstddef>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include <string>

#include "analysis/metrics.h"
#include "analysis/ptpn_analysis.h"
#include "json/json.h"
#include "petri/export_dot.h"
#include "petri/petri.h"
#include "tdg/tdg.h"
#include "tdg2pn/tdg2pn.h"
#include "tdg2ptopner/tdg2ptopner.h"
#include "tdg2ptopner/validate.h"
#include "tdg_test_helpers.h"

namespace {

using ptpn_test::load_tdg;

// Golden outputs are generated with libstdc++ (the stdlib used by CI):
//   ./build/ptpn tdg -f example/motivating-examples/hw.json \
//     --export-ptpn test/golden/hw.ptpn.dot --export-scg test/golden/hw.scg.dot \
//     --export-metrics test/golden/hw.metrics.json
//   ./build/ptpn tdg -f example/s-bench/a.json \
//     --export-ptpn test/golden/a.ptpn.dot --export-metrics test/golden/a.metrics.json
//   ./build/ptpn export ptopner -f example/p-bench/no-period.json \
//     -o test/golden/no-period.ppn

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
    {"example/p-bench/same-core.json", 20, 21, 631, 811, 181},
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
  ASSERT_TRUE(graph.save_to_dot(out));
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
    ASSERT_TRUE(state_class::MetricsAnalyzer::save_to_json(report, out));
    expect_matches_golden(out, test_case.second);
  }
}

TEST(PipelineGolden, PtopnerPpnMatchesGolden) {
  tdg::TDG tdg = load_tdg("example/p-bench/no-period.json");
  tdg.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;

  const auto validation = ptopner_export::validate_for_ptopner(tdg);
  ASSERT_TRUE(validation.ok);

  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);

  const std::string out = testing::TempDir() + "ptpn_test_ppn.ppn";
  const auto result = ptopner_export::export_ptpn_to_ppn_file(ptpn, out);
  ASSERT_TRUE(result.success) << result.error_message;
  expect_matches_golden(out, "test/golden/no-period.ppn");
}
