#include <gtest/gtest.h>

#include "analysis/metrics.h"
#include "analysis/ptpn_analysis.h"
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

  ptpn.node_pn_map["T"] = {petri::NodeRef::of(entry), petri::NodeRef::of(get_core),
                           petri::NodeRef::of(ready), petri::NodeRef::of(exec),
                           petri::NodeRef::of(exit)};
  ptpn.node_start_end_map["T"] = {petri::NodeRef::of(entry), petri::NodeRef::of(exit)};

  petri::TaskInfo info;
  info.core = 0;
  info.priority = 1;
  info.wcet = 5;
  info.bcet = 3;
  info.period = 0;
  info.deadline = 0;
  ptpn.task_info["T"] = info;
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

  net.node_pn_map["U"] = {petri::NodeRef::of(entry), petri::NodeRef::of(get_core),
                          petri::NodeRef::of(ready)};
  net.node_start_end_map["U"] = {petri::NodeRef::of(entry), petri::NodeRef::of(ready)};
  petri::TaskInfo info;
  info.core = 0;
  net.task_info["U"] = info;

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

}  // namespace
