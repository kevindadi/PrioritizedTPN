#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "model/petri.h"

namespace {

using state_class::HorizonInfo;
using state_class::MetricsAnalyzer;
using state_class::MetricsReport;
using state_class::StateClassReachabilityGraph;

// Periodic single task A on core 0:
//
//   A_period --[50,50]--> A_period + A_entry          (periodic release)
//   A_entry --get_core--> A_ready --exec1[2,3]--> A_mid1
//   A_mid1 --exec2[4,5]--> A_mid2 --exec3[1,2]--> A_exit
//   A_exit --consume--> (drained)
//
// One activation has a known WCRT interval [7, 10]: the dwell bounds of the
// three execution segments sum to 2+4+1 and 3+5+2 respectively. The period is
// long enough that no second release overlaps the first activation, so the
// periodic loop closes back onto the initial class.
petri::PTPN make_periodic_task_net() {
  petri::PTPN net;
  const petri::PlaceId period = net.add_place("A_period", 1);
  const petri::PlaceId entry = net.add_place("Aentry", 1);
  const petri::PlaceId ready = net.add_place("Aready", 1);
  const petri::PlaceId mid1 = net.add_place("Amid1", 1);
  const petri::PlaceId mid2 = net.add_place("Amid2", 1);
  const petri::PlaceId exit = net.add_place("Aexit", 1);
  net.set_initial_marking(period, 1);
  net.set_initial_marking(entry, 1);

  const petri::TransitionId fire =
      net.add_transition("A_fire", petri::TimeInterval(50, 50), /*priority=*/0, /*core=*/-1,
                         /*suspendable=*/false);
  net.set_pre_arc(period, fire, 1);
  net.set_post_arc(fire, period, 1);
  net.set_post_arc(fire, entry, 1);
  net.set_periodic_transition("A", fire);

  const petri::TransitionId get_core =
      net.add_transition("Aget_core", petri::TimeInterval(0, 0), /*priority=*/1, /*core=*/0,
                         /*suspendable=*/true);
  const petri::TransitionId exec1 =
      net.add_transition("Aexec1", petri::TimeInterval(2, 3), /*priority=*/1, /*core=*/0,
                         /*suspendable=*/true);
  const petri::TransitionId exec2 =
      net.add_transition("Aexec2", petri::TimeInterval(4, 5), /*priority=*/1, /*core=*/0,
                         /*suspendable=*/true);
  const petri::TransitionId exec3 =
      net.add_transition("Aexec3", petri::TimeInterval(1, 2), /*priority=*/1, /*core=*/0,
                         /*suspendable=*/true);
  const petri::TransitionId consume =
      net.add_transition("Aconsume", petri::TimeInterval(0, 0), /*priority=*/0, /*core=*/-1,
                         /*suspendable=*/false);

  net.set_pre_arc(entry, get_core, 1);
  net.set_post_arc(get_core, ready, 1);
  net.set_pre_arc(ready, exec1, 1);
  net.set_post_arc(exec1, mid1, 1);
  net.set_pre_arc(mid1, exec2, 1);
  net.set_post_arc(exec2, mid2, 1);
  net.set_pre_arc(mid2, exec3, 1);
  net.set_post_arc(exec3, exit, 1);
  net.set_pre_arc(exit, consume, 1);  // token is drained, keeping the net bounded

  net.set_core_parallelism(0, 1);
  net.set_task_chain(
      "A", {petri::NodeRef::of(entry), petri::NodeRef::of(get_core), petri::NodeRef::of(ready),
            petri::NodeRef::of(exec1), petri::NodeRef::of(mid1), petri::NodeRef::of(exec2),
            petri::NodeRef::of(mid2), petri::NodeRef::of(exec3), petri::NodeRef::of(exit)});
  net.set_node_span("A", petri::NodeRef::of(entry), petri::NodeRef::of(exit));

  petri::TaskInfo info;
  info.core = 0;
  info.priority = 1;
  info.wcet = 3 + 5 + 2;
  info.bcet = 2 + 4 + 1;
  info.period = 50;
  info.deadline = 50;
  net.set_task_info("A", info);
  return net;
}

MetricsReport analyze(const StateClassReachabilityGraph& graph, const petri::PTPN& net,
                      bool exact) {
  MetricsAnalyzer analyzer(graph.get_graph(), net, graph.get_initial_vertex(), exact);
  return analyzer.analyze();
}

TEST(TimeHorizonTest, LargeHorizonReproducesExhaustiveWcrt) {
  const petri::PTPN net = make_periodic_task_net();

  StateClassReachabilityGraph exhaustive(net);
  const size_t exhaustive_states = exhaustive.build(10000);
  const MetricsReport exhaustive_report = analyze(exhaustive, net, /*exact=*/true);
  ASSERT_EQ(exhaustive_report.tasks.size(), 1u);
  ASSERT_FALSE(exhaustive.get_statistics().truncated);
  const auto& baseline = exhaustive_report.tasks.front();
  // Known interval for the three execution segments.
  EXPECT_EQ(baseline.wcrt.value, 10);
  EXPECT_EQ(baseline.wcrt_lb.value, 7);
  EXPECT_FALSE(baseline.wcrt.infinite);

  StateClassReachabilityGraph bounded(net);
  bounded.set_time_horizon(1000);  // safely above the WCRT of interest
  const size_t bounded_states = bounded.build(10000);
  const auto& stats = bounded.get_statistics();
  EXPECT_FALSE(stats.truncated);
  EXPECT_FALSE(stats.horizon_truncated);
  EXPECT_EQ(stats.horizon_pruned_states, 0u);
  EXPECT_EQ(bounded_states, exhaustive_states);
  EXPECT_EQ(stats.total_transitions, exhaustive.get_statistics().total_transitions);

  MetricsAnalyzer analyzer(bounded.get_graph(), net, bounded.get_initial_vertex(), /*exact=*/true);
  HorizonInfo info;
  info.enabled = true;
  info.bound = 1000;
  info.truncated = stats.horizon_truncated;
  info.pruned_states = stats.horizon_pruned_states;
  analyzer.set_horizon_info(info);
  const MetricsReport bounded_report = analyzer.analyze();
  ASSERT_EQ(bounded_report.tasks.size(), 1u);
  const auto& observed = bounded_report.tasks.front();
  EXPECT_TRUE(bounded_report.exact);
  EXPECT_TRUE(bounded_report.deadlock_analysis_certified);
  EXPECT_EQ(observed.wcrt.value, baseline.wcrt.value);
  EXPECT_EQ(observed.wcrt_lb.value, baseline.wcrt_lb.value);
  EXPECT_EQ(observed.bcrt.value, baseline.bcrt.value);
  EXPECT_EQ(observed.wcrt_interval_tight, baseline.wcrt_interval_tight);
}

TEST(TimeHorizonTest, SmallHorizonReportsTruncationInsteadOfExactness) {
  const petri::PTPN net = make_periodic_task_net();

  StateClassReachabilityGraph exhaustive(net);
  exhaustive.build(10000);
  const MetricsReport exhaustive_report = analyze(exhaustive, net, /*exact=*/true);
  const auto& baseline = exhaustive_report.tasks.front();
  ASSERT_EQ(baseline.wcrt.value, 10);

  StateClassReachabilityGraph bounded(net);
  bounded.set_time_horizon(5);  // below the first activation's lower-bound WCRT
  const size_t bounded_states = bounded.build(10000);
  const auto& stats = bounded.get_statistics();
  EXPECT_FALSE(stats.truncated);
  EXPECT_TRUE(stats.horizon_truncated);
  EXPECT_GT(stats.horizon_pruned_states, 0u);
  EXPECT_LT(bounded_states, exhaustive.get_statistics().total_states);

  MetricsAnalyzer analyzer(bounded.get_graph(), net, bounded.get_initial_vertex(), /*exact=*/false);
  HorizonInfo info;
  info.enabled = true;
  info.bound = 5;
  info.truncated = stats.horizon_truncated;
  info.pruned_states = stats.horizon_pruned_states;
  analyzer.set_horizon_info(info);
  const MetricsReport report = analyzer.analyze();

  EXPECT_FALSE(report.exact);
  EXPECT_FALSE(report.deadlock_analysis_certified);
  EXPECT_TRUE(report.deadlock_states.empty());
  EXPECT_FALSE(report.schedulable);  // not proven; explicitly uncertified
  EXPECT_TRUE(report.horizon.enabled);
  EXPECT_TRUE(report.horizon.truncated);
  EXPECT_EQ(report.horizon.bound, 5);
  EXPECT_EQ(report.horizon.pruned_states, stats.horizon_pruned_states);
  ASSERT_EQ(report.tasks.size(), 1u);
  // The completion edge is beyond the horizon, so at best an incomplete path
  // is reported; it must never be presented as the exact exhaustive value.
  EXPECT_LT(report.tasks.front().wcrt.value, baseline.wcrt.value);
  EXPECT_FALSE(report.has_steady_cycle);
  EXPECT_EQ(report.recurrent_scc_size, 0u);

  const std::string path = testing::TempDir() + "ptpn_test_horizon.metrics.json";
  ASSERT_TRUE(MetricsAnalyzer::save_to_json(report, path));
  std::ifstream in(path);
  ASSERT_TRUE(in.is_open());
  const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(json.find("\"exact\": false"), std::string::npos);
  EXPECT_NE(json.find("\"schedulable\": null"), std::string::npos);
  EXPECT_NE(json.find("\"horizon\": {"), std::string::npos);
  EXPECT_NE(json.find("\"truncated\": true"), std::string::npos);
  EXPECT_NE(json.find("\"deadlock_analysis_certified\": false"), std::string::npos);
}

TEST(TimeHorizonTest, NegativeHorizonIsRejected) {
  const petri::PTPN net = make_periodic_task_net();
  StateClassReachabilityGraph graph(net);
  EXPECT_THROW(graph.set_time_horizon(-1), std::invalid_argument);
  EXPECT_FALSE(graph.has_time_horizon());
}

}  // namespace
