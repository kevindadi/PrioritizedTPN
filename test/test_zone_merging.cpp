#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "model/petri.h"

namespace {

using state_class::MetricsAnalyzer;
using state_class::MetricsReport;
using state_class::ReductionMode;
using state_class::StateClassReachabilityGraph;

std::set<std::vector<int>> reachable_markings(const state_class::SCGraph& graph) {
  std::set<std::vector<int>> markings;
  for (auto [it, end] = boost::vertices(graph); it != end; ++it) {
    markings.insert(boost::get(boost::vertex_name, graph, *it).marking);
  }
  return markings;
}

// Two independent timed transitions start together and can fire in either
// order:
//   a_exec: window [1, 5]   (transition index 0, explored first)
//   b_exec: window [4, 10]  (transition index 1)
// A long observer transition c_exec [100, 100] stays enabled throughout.
//
// Firing a_exec then b_exec leaves the observer clock in [4, 10]; firing
// b_exec then a_exec forces both firings into [4, 5]. Both orders reach the
// same marking and selected active set ({c_exec}), and the second zone is
// strictly included in the first. Breadth-first exploration visits the larger
// zone first, so the candidate with the smaller zone is merged into it.
petri::PTPN make_merging_net() {
  petri::PTPN ptpn;
  const petri::PlaceId pa = ptpn.add_place("pa", 1);
  const petri::PlaceId pb = ptpn.add_place("pb", 1);
  const petri::PlaceId p_obs = ptpn.add_place("p_obs", 1);
  const petri::PlaceId da = ptpn.add_place("da", 1);
  const petri::PlaceId db = ptpn.add_place("db", 1);
  const petri::PlaceId done = ptpn.add_place("done", 1);
  ptpn.set_initial_marking(pa, 1);
  ptpn.set_initial_marking(pb, 1);
  ptpn.set_initial_marking(p_obs, 1);

  const petri::TransitionId a_exec = ptpn.add_transition(
      "a_exec", petri::TimeInterval(1, 5), 0, petri::kControlTransitionCore);
  const petri::TransitionId b_exec = ptpn.add_transition(
      "b_exec", petri::TimeInterval(4, 10), 0, petri::kControlTransitionCore);
  const petri::TransitionId c_exec = ptpn.add_transition(
      "c_exec", petri::TimeInterval(100, 100), 0, petri::kControlTransitionCore);

  ptpn.set_pre_arc(pa, a_exec, 1);
  ptpn.set_post_arc(a_exec, da, 1);
  ptpn.set_pre_arc(pb, b_exec, 1);
  ptpn.set_post_arc(b_exec, db, 1);
  ptpn.set_pre_arc(p_obs, c_exec, 1);
  ptpn.set_post_arc(c_exec, done, 1);

  // Metrics topology: task T is released at p_obs and completes at done, so
  // its WCRT is the time from the initial state until c_exec fires.
  ptpn.set_task_chain(
      "T", {petri::NodeRef::of(p_obs), petri::NodeRef::of(c_exec), petri::NodeRef::of(done)});
  ptpn.set_node_span("T", petri::NodeRef::of(p_obs), petri::NodeRef::of(done));
  petri::TaskInfo info;
  info.core = petri::kControlTransitionCore;
  info.priority = 0;
  info.wcet = 100;
  info.bcet = 100;
  ptpn.set_task_info("T", info);
  return ptpn;
}

MetricsReport analyze(const StateClassReachabilityGraph& graph, const petri::PTPN& net,
                      bool exact, bool wcrt_lb_valid) {
  MetricsAnalyzer analyzer(graph.get_graph(), net, graph.get_initial_vertex(), exact,
                           wcrt_lb_valid);
  return analyzer.analyze();
}

// A time value is "at most" another when it is finite and no larger, or the
// other is infinite.
bool time_at_most(const state_class::TimeValue& lhs, const state_class::TimeValue& rhs) {
  if (rhs.infinite) {
    return true;
  }
  return !lhs.infinite && lhs.value <= rhs.value;
}

TEST(ZoneMergingTest, MergesIncludedZoneIntoExistingState) {
  const petri::PTPN net = make_merging_net();

  StateClassReachabilityGraph exact(net);
  exact.set_reduction_mode(ReductionMode::NONE);
  const size_t exact_states = exact.build(10000);
  ASSERT_FALSE(exact.get_statistics().truncated);

  StateClassReachabilityGraph merged(net);
  merged.set_reduction_mode(ReductionMode::UPPER_BOUND);
  const size_t merged_states = merged.build(10000);
  ASSERT_FALSE(merged.get_statistics().truncated);

  // The reduction is active and only removes states.
  EXPECT_LT(merged_states, exact_states);
  EXPECT_LE(merged.get_statistics().total_states, exact.get_statistics().total_states);
  EXPECT_GT(merged.get_statistics().zone_merges, 0u);
  EXPECT_GT(merged.get_statistics().merged_states, 0u);
  EXPECT_LE(merged.get_statistics().merged_states,
            merged.get_statistics().zone_merges);

  // Every marking of the exact graph is still represented (over-approximation
  // must not delete behaviors, even though it drops the tighter zone).
  EXPECT_EQ(reachable_markings(merged.get_graph()), reachable_markings(exact.get_graph()));

  // WCRT from the merged graph is a sound upper bound: the exact value never
  // exceeds it.
  const MetricsReport exact_report = analyze(exact, net, /*exact=*/true,
                                             /*wcrt_lb_valid=*/true);
  const MetricsReport merged_report = analyze(merged, net, /*exact=*/false,
                                              /*wcrt_lb_valid=*/false);
  ASSERT_EQ(exact_report.tasks.size(), merged_report.tasks.size());
  ASSERT_EQ(merged_report.tasks.size(), 1u);
  EXPECT_FALSE(merged_report.exact);
  EXPECT_FALSE(merged_report.wcrt_lb_valid);
  const auto& exact_wcrt = exact_report.tasks.front().wcrt;
  const auto& merged_wcrt = merged_report.tasks.front().wcrt;
  EXPECT_TRUE(time_at_most(exact_wcrt, merged_wcrt))
      << "exact=" << exact_wcrt.to_string() << " merged=" << merged_wcrt.to_string();
}

TEST(ZoneMergingTest, MetricsJsonNullsTheUncertifiedLowerBound) {
  const petri::PTPN net = make_merging_net();
  StateClassReachabilityGraph merged(net);
  merged.set_reduction_mode(ReductionMode::UPPER_BOUND);
  merged.build(10000);
  const MetricsReport report = analyze(merged, net, /*exact=*/false,
                                       /*wcrt_lb_valid=*/false);

  const std::string path = testing::TempDir() + "ptpn_zone_merging_metrics.json";
  ASSERT_TRUE(MetricsAnalyzer::save_to_json(report, path));

  std::ifstream in(path);
  ASSERT_TRUE(in.is_open());
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string json = buffer.str();

  EXPECT_NE(json.find("\"exact\": false"), std::string::npos);
  EXPECT_NE(json.find("\"wcrt_lb_valid\": false"), std::string::npos);
  EXPECT_NE(json.find("\"wcrt_lb\": null"), std::string::npos);
  EXPECT_NE(json.find("\"wcrt_interval_tight\": false"), std::string::npos);
}

// Equal-priority ties on a real core produce several admissible active sets
// for the same marking. Upper-bound merging must keep them separate: the
// bucket key includes the selected active set, and check_inclusion requires
// layout equality.
TEST(ZoneMergingTest, NeverMergesAcrossDifferentActiveSets) {
  petri::PTPN ptpn;
  const petri::PlaceId p0 = ptpn.add_place("p0", 1);
  const petri::PlaceId p1 = ptpn.add_place("p1", 1);
  ptpn.set_initial_marking(p0, 1);
  ptpn.set_initial_marking(p1, 1);

  // Self-loops keep the marking constant while the clocks advance.
  const petri::TransitionId t0 =
      ptpn.add_transition("t0", petri::TimeInterval(1, 2), 1, /*core=*/0);
  const petri::TransitionId t1 =
      ptpn.add_transition("t1", petri::TimeInterval(1, 2), 1, /*core=*/0);
  ptpn.set_pre_arc(p0, t0, 1);
  ptpn.set_post_arc(t0, p0, 1);
  ptpn.set_pre_arc(p1, t1, 1);
  ptpn.set_post_arc(t1, p1, 1);
  ptpn.set_core_parallelism(0, 1);

  StateClassReachabilityGraph merged(ptpn);
  merged.set_reduction_mode(ReductionMode::UPPER_BOUND);
  merged.build(64);

  // Two initial classes, one per equal-priority tie resolution, sharing the
  // marking but selecting different active sets.
  const state_class::SCGraph& graph = merged.get_graph();
  EXPECT_GE(boost::num_vertices(graph), 2u);
  std::set<state_class::TransitionSet> active_sets;
  for (auto [it, end] = boost::vertices(graph); it != end; ++it) {
    const state_class::StateClass& state = boost::get(boost::vertex_name, graph, *it);
    if (state.marking == ptpn.get_marking()) {
      active_sets.insert(state.priority_enabled);
    }
  }
  EXPECT_GE(active_sets.size(), 2u);
}

// Directly exercise the guard used by the merge path: zone inclusion alone is
// not sufficient, the selected active set must match as well.
TEST(ZoneMergingTest, InclusionRequiresSameActiveSet) {
  state_class::StateClass wide;
  wide.marking = {1};
  wide.clock_vars = {{state_class::ClockKind::Zero, state_class::NO_TRANSITION},
                     {state_class::ClockKind::Execution, 0}};
  wide.exec_clock_of_transition = {1};
  wide.susp_clock_of_transition = {-1};
  wide.priority_enabled = {0};
  wide.zone = state_class::DBM(2);
  wide.zone.set_constraint(1, 0, 10);
  wide.zone.minimize();

  state_class::StateClass narrow = wide;
  narrow.zone.set_constraint(1, 0, 5);
  narrow.zone.minimize();

  ASSERT_TRUE(narrow.zone.included_in(wide.zone));
  EXPECT_TRUE(state_class::check_inclusion(narrow, wide));

  narrow.priority_enabled = {1};
  EXPECT_FALSE(state_class::check_inclusion(narrow, wide));
}

}  // namespace
