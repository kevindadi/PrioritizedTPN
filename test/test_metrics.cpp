#include <gtest/gtest.h>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "model/petri.h"

namespace {

using state_class::MetricsAnalyzer;
using state_class::MetricsReport;
using state_class::StateClassReachabilityGraph;

// Single task T on core 0: entry -(get_core,[0,0])-> ready -(exec,[3,5])-> exit
// -(consume,[0,0])-> done. exec is the only timed segment, so the response time
// of the lone activation is exactly the execution window [3, 5].
petri::PTPN make_single_task_net(bool with_consume) {
  petri::PTPN ptpn;
  const petri::PlaceId entry = ptpn.add_place("Tentry", 1);
  const petri::TransitionId get_core = ptpn.add_transition("Tget_core", petri::TimeInterval(0, 0),
                                                           /*priority=*/1, /*core=*/0, false);
  const petri::PlaceId ready = ptpn.add_place("Tready", 1);
  const petri::TransitionId exec = ptpn.add_transition("Texec", petri::TimeInterval(3, 5),
                                                       /*priority=*/1, /*core=*/0, true);
  const petri::PlaceId exit = ptpn.add_place("Texit", 1);

  ptpn.set_initial_marking(entry, 1);
  ptpn.set_pre_arc(entry, get_core, 1);
  ptpn.set_post_arc(get_core, ready, 1);
  ptpn.set_pre_arc(ready, exec, 1);
  ptpn.set_post_arc(exec, exit, 1);

  if (with_consume) {
    const petri::PlaceId done = ptpn.add_place("Tdone", 1);
    const petri::TransitionId consume =
        ptpn.add_transition("Tconsume", petri::TimeInterval(0, 0), 0, -1, false);
    ptpn.set_pre_arc(exit, consume, 1);
    ptpn.set_post_arc(consume, done, 1);
  }

  ptpn.set_task_chain(
      "T", {petri::NodeRef::of(entry), petri::NodeRef::of(get_core), petri::NodeRef::of(ready),
            petri::NodeRef::of(exec), petri::NodeRef::of(exit)});
  ptpn.set_node_span("T", petri::NodeRef::of(entry), petri::NodeRef::of(exit));

  petri::TaskInfo info;
  info.core = 0;
  info.priority = 1;
  info.wcet = 5;
  info.bcet = 3;
  info.period = 0;
  info.deadline = 0;
  ptpn.set_task_info("T", info);
  return ptpn;
}

MetricsReport analyze(const petri::PTPN& net) {
  StateClassReachabilityGraph graph(net);
  graph.build(256);
  MetricsAnalyzer analyzer(graph.get_graph(), net, graph.get_initial_vertex(),
                           /*exact=*/true);
  return analyzer.analyze();
}

TEST(MetricsTest, SingleTaskResponseTimeMatchesExecutionWindow) {
  const MetricsReport report = analyze(make_single_task_net(true));

  ASSERT_EQ(report.tasks.size(), 1u);
  const auto& t = report.tasks.front();
  EXPECT_EQ(t.name, "T");
  EXPECT_TRUE(t.observed);
  EXPECT_EQ(t.activations, 1);
  EXPECT_FALSE(t.wcrt.infinite);
  EXPECT_EQ(t.wcrt.value, 5);
  EXPECT_FALSE(t.bcrt.infinite);
  EXPECT_EQ(t.bcrt.value, 3);
  EXPECT_EQ(t.jitter.value, 2);
  EXPECT_EQ(t.max_in_flight, 1);
  EXPECT_EQ(t.max_preemptions, 0);

  EXPECT_TRUE(report.bounded);
  EXPECT_TRUE(report.schedulable);
  EXPECT_TRUE(report.deadlock_states.empty());
}

TEST(MetricsTest, StuckTaskIsReportedAsDeadlock) {
  // No consume transition: the activation finishes but the exit token is never
  // drained, so the terminal class still carries chain work and is flagged.
  petri::PTPN net;
  const petri::PlaceId entry = net.add_place("Uentry", 1);
  const petri::TransitionId get_core =
      net.add_transition("Uget_core", petri::TimeInterval(0, 0), 1, 0, false);
  const petri::PlaceId ready = net.add_place("Uready", 1);
  // A CPU resource that is never available: get_core can never fire.
  const petri::PlaceId cpu = net.add_place("cpu", 1);
  net.set_initial_marking(entry, 1);
  net.set_pre_arc(entry, get_core, 1);
  net.set_pre_arc(cpu, get_core, 1);  // cpu has 0 tokens -> permanently blocked
  net.set_post_arc(get_core, ready, 1);

  net.set_task_chain(
      "U", {petri::NodeRef::of(entry), petri::NodeRef::of(get_core), petri::NodeRef::of(ready)});
  net.set_node_span("U", petri::NodeRef::of(entry), petri::NodeRef::of(ready));
  petri::TaskInfo info;
  info.core = 0;
  net.set_task_info("U", info);

  const MetricsReport report = analyze(net);
  EXPECT_FALSE(report.deadlock_states.empty());
}

TEST(MetricsTest, NetWithoutTaskInfoYieldsStructuralOnly) {
  petri::PTPN net;
  const petri::PlaceId in = net.add_place("in", 1);
  const petri::PlaceId done = net.add_place("done", 1);
  const petri::TransitionId t = net.add_transition("t", petri::TimeInterval(0, 2), 0, -1);
  net.set_initial_marking(in, 1);
  net.set_pre_arc(in, t, 1);
  net.set_post_arc(t, done, 1);

  const MetricsReport report = analyze(net);
  EXPECT_TRUE(report.tasks.empty());
  EXPECT_TRUE(report.bounded);
  EXPECT_GE(report.states, 1u);
}

TEST(MetricsTest, WcrtCertifiedIntervalBracketsTheExactValue) {
  // The DP upper bound and the certified lower bound bracket the true WCRT;
  // for an unconstrained execution window [3,5] they are 5 and 3.
  const MetricsReport report = analyze(make_single_task_net(true));

  ASSERT_EQ(report.tasks.size(), 1u);
  const auto& t = report.tasks.front();
  EXPECT_EQ(t.wcrt.value, 5);
  EXPECT_EQ(t.wcrt_lb.value, 3);
  EXPECT_LE(t.wcrt_lb.value, t.wcrt.value);
  EXPECT_FALSE(t.wcrt_interval_tight);
}

// High-priority task H keeps core 0 busy forever (period 2, execution 2),
// while low-priority task L holds a token in its entry place and can never be
// dispatched. The SCG has a positive-time cycle with an edge labelled by H's
// periodic release, which is exactly a periodic local deadlock for L.
petri::PTPN make_starved_task_net() {
  petri::PTPN net;
  const petri::PlaceId h_period = net.add_place("H_period", 1, true);
  const petri::PlaceId h_entry = net.add_place("Hentry", 1);
  const petri::PlaceId h_ready = net.add_place("Hready", 1);
  const petri::PlaceId h_exit = net.add_place("Hexit", 1);
  net.set_initial_marking(h_period, 1);
  net.set_initial_marking(h_entry, 1);

  const petri::TransitionId h_fire =
      net.add_transition("H_fire", petri::TimeInterval(2, 2), 0, -1, false);
  net.set_pre_arc(h_period, h_fire, 1);
  net.set_post_arc(h_fire, h_period, 1);
  net.set_post_arc(h_fire, h_entry, 1);
  net.set_periodic_transition("H", h_fire);

  const petri::TransitionId h_get =
      net.add_transition("Hget_core", petri::TimeInterval(0, 0), 10, 0, false);
  net.set_pre_arc(h_entry, h_get, 1);
  net.set_post_arc(h_get, h_ready, 1);

  const petri::TransitionId h_exec =
      net.add_transition("Hexec", petri::TimeInterval(2, 2), 10, 0, false);
  net.set_pre_arc(h_ready, h_exec, 1);
  net.set_post_arc(h_exec, h_exit, 1);

  const petri::TransitionId h_consume =
      net.add_transition("Hconsume", petri::TimeInterval(0, 0), 0, -1, false);
  net.set_pre_arc(h_exit, h_consume, 1);  // token is consumed, keeping the net bounded

  const petri::PlaceId l_entry = net.add_place("Lentry", 1);
  const petri::PlaceId l_ready = net.add_place("Lready", 1);
  const petri::PlaceId l_exit = net.add_place("Lexit", 1);
  net.set_initial_marking(l_entry, 1);

  const petri::TransitionId l_get =
      net.add_transition("Lget_core", petri::TimeInterval(0, 0), 1, 0, false);
  net.set_pre_arc(l_entry, l_get, 1);
  net.set_post_arc(l_get, l_ready, 1);

  const petri::TransitionId l_exec =
      net.add_transition("Lexec", petri::TimeInterval(1, 1), 1, 0, false);
  net.set_pre_arc(l_ready, l_exec, 1);
  net.set_post_arc(l_exec, l_exit, 1);

  net.set_core_parallelism(0, 1);
  net.set_task_chain("L", {petri::NodeRef::of(l_entry), petri::NodeRef::of(l_get),
                           petri::NodeRef::of(l_ready), petri::NodeRef::of(l_exec),
                           petri::NodeRef::of(l_exit)});
  net.set_node_span("L", petri::NodeRef::of(l_entry), petri::NodeRef::of(l_exit));
  petri::TaskInfo info;
  info.core = 0;
  info.priority = 1;
  net.set_task_info("L", info);
  return net;
}

TEST(MetricsTest, PeriodicLocalDeadlockIsDetected) {
  const MetricsReport report = analyze(make_starved_task_net());

  ASSERT_EQ(report.tasks.size(), 1u);
  const auto& t = report.tasks.front();
  EXPECT_TRUE(t.local_deadlock);
  EXPECT_GT(t.local_deadlock_scc_size, 0u);
  EXPECT_FALSE(report.schedulable);
}

}  // namespace
