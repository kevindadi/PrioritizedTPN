#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "tdg_test_helpers.h"

namespace {

using state_class::MetricsAnalyzer;
using state_class::MetricsReport;
using state_class::StateClassReachabilityGraph;
using state_class::TaskMetrics;
using state_class::TimeValue;

struct RunResult {
  MetricsReport report;
  StateClassReachabilityGraph::Statistics stats;
  size_t states = 0;
};

RunResult run_analysis(const petri::PTPN& net, bool por, size_t max_states) {
  StateClassReachabilityGraph graph(net);
  graph.set_por_enabled(por);
  const size_t states = graph.build(max_states);
  MetricsAnalyzer analyzer(graph.get_graph(), net, graph.get_initial_vertex(),
                           /*exact=*/true);
  return {analyzer.analyze(), graph.get_statistics(), states};
}

void expect_time_equal(const TimeValue& full, const TimeValue& reduced,
                       const std::string& what) {
  EXPECT_EQ(full.infinite, reduced.infinite) << what;
  if (!full.infinite && !reduced.infinite) {
    EXPECT_EQ(full.value, reduced.value) << what;
  }
}

// Compares every per-task interval reported by the full and reduced builds.
void expect_same_task_metrics(const MetricsReport& full, const MetricsReport& reduced) {
  std::map<std::string, const TaskMetrics*> by_name;
  for (const TaskMetrics& task : full.tasks) {
    by_name[task.name] = &task;
  }
  ASSERT_EQ(full.tasks.size(), reduced.tasks.size());
  for (const TaskMetrics& task : reduced.tasks) {
    const auto it = by_name.find(task.name);
    ASSERT_NE(it, by_name.end()) << "task " << task.name << " missing from full report";
    const TaskMetrics& expected = *it->second;
    expect_time_equal(expected.wcrt, task.wcrt, task.name + ".wcrt");
    expect_time_equal(expected.wcrt_lb, task.wcrt_lb, task.name + ".wcrt_lb");
    expect_time_equal(expected.bcrt, task.bcrt, task.name + ".bcrt");
    expect_time_equal(expected.jitter, task.jitter, task.name + ".jitter");
    EXPECT_EQ(expected.local_deadlock, task.local_deadlock) << task.name;
    EXPECT_EQ(expected.deadline_missed, task.deadline_missed) << task.name;
  }
  // Vertex ids are graph-local; the deadlock count is the observable property.
  EXPECT_EQ(full.deadlock_states.size(), reduced.deadlock_states.size());
  EXPECT_EQ(full.bounded, reduced.bounded);
  EXPECT_EQ(full.schedulable, reduced.schedulable);
}

void add_task_chain(petri::PTPN& net, const std::string& name,
                    const std::vector<petri::NodeRef>& chain, int core, int priority,
                    int wcet, int bcet) {
  net.set_task_chain(name, chain);
  net.set_node_span(name, chain.front(), chain.back());
  petri::TaskInfo info;
  info.core = core;
  info.priority = priority;
  info.wcet = wcet;
  info.bcet = bcet;
  info.period = 0;
  info.deadline = 0;
  net.set_task_info(name, info);
}

// Two independent task chains on cores 0 and 1 plus three independent,
// zero-delay-at-this-class transitions: two on the control core and one on a
// spare real core. The urgent [0,0] setup transitions pin elapsed time, so the
// three extra transitions fire with zero dwell here and are commutable.
petri::PTPN make_independent_control_net() {
  petri::PTPN net;
  const petri::PlaceId u_entry = net.add_place("Uentry", 1);
  const petri::PlaceId u_ready = net.add_place("Uready", 1);
  const petri::PlaceId u_exit = net.add_place("Uexit", 1);
  const petri::PlaceId v_entry = net.add_place("Ventry", 1);
  const petri::PlaceId v_ready = net.add_place("Vready", 1);
  const petri::PlaceId v_exit = net.add_place("Vexit", 1);
  const petri::PlaceId c0_in = net.add_place("c0_in", 1);
  const petri::PlaceId c0_out = net.add_place("c0_out", 1);
  const petri::PlaceId c1_in = net.add_place("c1_in", 1);
  const petri::PlaceId c1_out = net.add_place("c1_out", 1);
  const petri::PlaceId aux_in = net.add_place("aux_in", 1);
  const petri::PlaceId aux_out = net.add_place("aux_out", 1);

  net.set_initial_marking(u_entry, 1);
  net.set_initial_marking(v_entry, 1);
  net.set_initial_marking(c0_in, 1);
  net.set_initial_marking(c1_in, 1);
  net.set_initial_marking(aux_in, 1);

  const petri::TransitionId u_get =
      net.add_transition("Uget_core", petri::TimeInterval(0, 0), 5, 0, false);
  const petri::TransitionId u_exec =
      net.add_transition("Uexec", petri::TimeInterval(2, 2), 5, 0, true);
  const petri::TransitionId v_get =
      net.add_transition("Vget_core", petri::TimeInterval(0, 0), 5, 1, false);
  const petri::TransitionId v_exec =
      net.add_transition("Vexec", petri::TimeInterval(3, 3), 5, 1, true);
  const petri::TransitionId c0 =
      net.add_transition("c0", petri::TimeInterval(0, 2), 0, -1, false);
  const petri::TransitionId c1 =
      net.add_transition("c1", petri::TimeInterval(0, 2), 0, -1, false);
  const petri::TransitionId aux =
      net.add_transition("aux", petri::TimeInterval(0, 2), 0, 2, false);

  net.set_pre_arc(u_entry, u_get, 1);
  net.set_post_arc(u_get, u_ready, 1);
  net.set_pre_arc(u_ready, u_exec, 1);
  net.set_post_arc(u_exec, u_exit, 1);
  net.set_pre_arc(v_entry, v_get, 1);
  net.set_post_arc(v_get, v_ready, 1);
  net.set_pre_arc(v_ready, v_exec, 1);
  net.set_post_arc(v_exec, v_exit, 1);
  net.set_pre_arc(c0_in, c0, 1);
  net.set_post_arc(c0, c0_out, 1);
  net.set_pre_arc(c1_in, c1, 1);
  net.set_post_arc(c1, c1_out, 1);
  net.set_pre_arc(aux_in, aux, 1);
  net.set_post_arc(aux, aux_out, 1);

  add_task_chain(net, "U",
                 {petri::NodeRef::of(u_entry), petri::NodeRef::of(u_get),
                  petri::NodeRef::of(u_ready), petri::NodeRef::of(u_exec),
                  petri::NodeRef::of(u_exit)},
                 0, 5, 2, 2);
  add_task_chain(net, "V",
                 {petri::NodeRef::of(v_entry), petri::NodeRef::of(v_get),
                  petri::NodeRef::of(v_ready), petri::NodeRef::of(v_exec),
                  petri::NodeRef::of(v_exit)},
                 1, 5, 3, 3);
  return net;
}

TEST(PorTest, IndependentControlTransitionsPreserveMetricsAndReduce) {
  const petri::PTPN net = make_independent_control_net();
  const RunResult full = run_analysis(net, /*por=*/false, 1024);
  const RunResult reduced = run_analysis(net, /*por=*/true, 1024);

  expect_same_task_metrics(full.report, reduced.report);
  ASSERT_EQ(full.report.tasks.size(), 2u);
  for (const TaskMetrics& task : reduced.report.tasks) {
    EXPECT_FALSE(task.local_deadlock);
    EXPECT_FALSE(task.wcrt.infinite);
  }

  // The reduced build must explore strictly fewer classes or edges.
  EXPECT_LT(reduced.states + reduced.stats.total_transitions,
            full.states + full.stats.total_transitions);
  EXPECT_GT(reduced.stats.persistent_set_calls, 0u);
  EXPECT_GT(reduced.stats.persistent_transitions_skipped, 0u);
}

TEST(PorTest, DependentTransitionsAreNotReduced) {
  // Shared input place: only one of the two control transitions may fire.
  petri::PTPN shared;
  const petri::PlaceId shared_in = shared.add_place("shared_in", 1);
  const petri::PlaceId out0 = shared.add_place("out0", 1);
  const petri::PlaceId out1 = shared.add_place("out1", 1);
  shared.set_initial_marking(shared_in, 1);
  const petri::TransitionId t0 =
      shared.add_transition("t0", petri::TimeInterval(0, 0), 0, -1, false);
  const petri::TransitionId t1 =
      shared.add_transition("t1", petri::TimeInterval(0, 0), 0, -1, false);
  shared.set_pre_arc(shared_in, t0, 1);
  shared.set_post_arc(t0, out0, 1);
  shared.set_pre_arc(shared_in, t1, 1);
  shared.set_post_arc(t1, out1, 1);

  StateClassReachabilityGraph shared_graph(shared);
  EXPECT_TRUE(shared_graph.transitions_dependent(t0.index(), t1.index()));
  shared_graph.set_por_enabled(true);
  shared_graph.build(64);
  EXPECT_EQ(shared_graph.get_statistics().persistent_transitions_skipped, 0u);

  // Same real core (two slots, so both are priority-enabled at once): the
  // relation must still mark them dependent and the reduction must keep both.
  petri::PTPN same_core;
  const petri::PlaceId in0 = same_core.add_place("in0", 1);
  const petri::PlaceId in1 = same_core.add_place("in1", 1);
  const petri::PlaceId done0 = same_core.add_place("done0", 1);
  const petri::PlaceId done1 = same_core.add_place("done1", 1);
  same_core.set_initial_marking(in0, 1);
  same_core.set_initial_marking(in1, 1);
  same_core.set_core_parallelism(0, 2);
  const petri::TransitionId r0 =
      same_core.add_transition("r0", petri::TimeInterval(0, 0), 1, 0, false);
  const petri::TransitionId r1 =
      same_core.add_transition("r1", petri::TimeInterval(0, 0), 1, 0, false);
  same_core.set_pre_arc(in0, r0, 1);
  same_core.set_post_arc(r0, done0, 1);
  same_core.set_pre_arc(in1, r1, 1);
  same_core.set_post_arc(r1, done1, 1);

  StateClassReachabilityGraph same_core_graph(same_core);
  EXPECT_TRUE(same_core_graph.transitions_dependent(r0.index(), r1.index()));
  same_core_graph.set_por_enabled(true);
  same_core_graph.build(64);
  EXPECT_EQ(same_core_graph.get_statistics().persistent_transitions_skipped, 0u);
}

petri::PTPN lower_to_ptpn(const std::string& path) {
  tdg::TDG tdg = ptpn_test::load_tdg(path);
  petri::PTPN net;
  converter::TDG2PN::transform(tdg, net);
  return net;
}

TEST(PorTest, FullAndReducedMetricsMatchOnExistingNets) {
  for (const char* path :
       {"example/motivating-examples/hw.json", "example/s-bench/a.json"}) {
    SCOPED_TRACE(path);
    const petri::PTPN net = lower_to_ptpn(path);
    const RunResult full = run_analysis(net, /*por=*/false, 5000);
    const RunResult reduced = run_analysis(net, /*por=*/true, 5000);

    EXPECT_FALSE(full.stats.truncated);
    EXPECT_FALSE(reduced.stats.truncated);
    expect_same_task_metrics(full.report, reduced.report);
    EXPECT_LE(reduced.stats.total_states, full.stats.total_states);
    EXPECT_LE(reduced.stats.total_transitions, full.stats.total_transitions);
  }
}

}  // namespace
