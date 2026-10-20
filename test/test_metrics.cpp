#include <cstddef>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/metrics/metrics_json.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "model/petri.h"

namespace {

using state_class::LockMetrics;
using state_class::MetricsAnalyzer;
using state_class::MetricsReport;
using state_class::StateClassReachabilityGraph;
using state_class::TaskMetrics;

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

TEST(MetricsTest, JsonEscapesTaskAndLockNames) {
  MetricsReport report;
  TaskMetrics task;
  task.name = "ta\"sk\\name\nline\x01" "end";
  report.tasks.push_back(task);
  LockMetrics lock;
  lock.name = "lo\"ck\\name\ttab";
  report.locks.push_back(lock);

  const std::string path = ::testing::TempDir() + "ptpn_test_metrics_escape.json";
  ASSERT_TRUE(state_class::save_metrics_json(report, path));

  std::ifstream in(path, std::ios::binary);
  ASSERT_TRUE(in.is_open());
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
  ASSERT_FALSE(parsed.is_discarded());

  ASSERT_EQ(parsed["tasks"].size(), 1u);
  EXPECT_EQ(parsed["tasks"][0]["name"].get<std::string>(), task.name);
  ASSERT_EQ(parsed["locks"].size(), 1u);
  EXPECT_EQ(parsed["locks"][0]["name"].get<std::string>(), lock.name);
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
  net.set_task_chain(
      "L", {petri::NodeRef::of(l_entry), petri::NodeRef::of(l_get), petri::NodeRef::of(l_ready),
            petri::NodeRef::of(l_exec), petri::NodeRef::of(l_exit)});
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

size_t place_index(const petri::PTPN& net, const std::string& name) {
  for (size_t p = 0; p < net.num_places(); ++p) {
    if (net.get_place(petri::PlaceId{p}).name == name) {
      return p;
    }
  }
  throw std::runtime_error("missing place " + name);
}

size_t transition_index(const petri::PTPN& net, const std::string& name) {
  for (size_t t = 0; t < net.num_transitions(); ++t) {
    if (net.get_transition(petri::TransitionId{t}).name == name) {
      return t;
    }
  }
  throw std::runtime_error("missing transition " + name);
}

state_class::SCVertex add_manual_state(state_class::SCGraph& graph, std::vector<int> marking,
                                       size_t id) {
  state_class::StateClass state;
  state.marking = std::move(marking);
  state.id = id;
  return boost::add_vertex(std::move(state), graph);
}

void add_manual_edge(state_class::SCGraph& graph, state_class::SCVertex from,
                     state_class::SCVertex to, int transition, int dwell_min, int dwell_max) {
  state_class::FiringEdge edge(transition, dwell_min, dwell_max, dwell_min, dwell_max);
  boost::add_edge(from, to, edge, graph);
}

// Two single-shot tasks with equal priority on one core, both released at
// t = 0. The scheduler cannot order them, so the initial active set has two
// admissible branches and the reachability builder creates two initial
// classes.
petri::PTPN make_two_equal_priority_start_tasks_net() {
  petri::PTPN net;
  auto add_task = [&](const std::string& name, int exec_time) {
    const petri::PlaceId entry = net.add_place(name + "entry", 1);
    const petri::TransitionId get_core = net.add_transition(
        name + "get_core", petri::TimeInterval(0, 0), /*priority=*/1, /*core=*/0, false);
    const petri::PlaceId ready = net.add_place(name + "ready", 1);
    const petri::TransitionId exec =
        net.add_transition(name + "exec", petri::TimeInterval(exec_time, exec_time),
                           /*priority=*/1, /*core=*/0, false);
    const petri::PlaceId exit = net.add_place(name + "exit", 1);
    const petri::PlaceId done = net.add_place(name + "done", 1);
    const petri::TransitionId consume =
        net.add_transition(name + "consume", petri::TimeInterval(0, 0), /*priority=*/0,
                           /*core=*/-1, false);

    net.set_initial_marking(entry, 1);
    net.set_pre_arc(entry, get_core, 1);
    net.set_post_arc(get_core, ready, 1);
    net.set_pre_arc(ready, exec, 1);
    net.set_post_arc(exec, exit, 1);
    net.set_pre_arc(exit, consume, 1);
    net.set_post_arc(consume, done, 1);

    net.set_task_chain(
        name, {petri::NodeRef::of(entry), petri::NodeRef::of(get_core), petri::NodeRef::of(ready),
               petri::NodeRef::of(exec), petri::NodeRef::of(exit)});
    net.set_node_span(name, petri::NodeRef::of(entry), petri::NodeRef::of(exit));

    petri::TaskInfo info;
    info.core = 0;
    info.priority = 1;
    info.wcet = exec_time;
    info.bcet = exec_time;
    net.set_task_info(name, info);
  };

  add_task("A", 4);
  add_task("B", 2);
  net.set_core_parallelism(0, 1);
  return net;
}

TEST(MetricsTest, EqualPriorityStartTasksBuildMultipleInitialClasses) {
  const petri::PTPN net = make_two_equal_priority_start_tasks_net();
  StateClassReachabilityGraph graph(net);
  graph.build(2048);
  EXPECT_FALSE(graph.get_statistics().truncated);

  ASSERT_EQ(graph.get_initial_vertices().size(), 2u);
  EXPECT_EQ(graph.get_initial_vertex(), graph.get_initial_vertices().front());

  // Both start tasks are released at t=0. Their response time is the worst
  // over both dispatch branches: A then B (4+2) or B then A (2+4).
  MetricsAnalyzer analyzer(graph.get_graph(), net, graph.get_initial_vertices(),
                           /*exact=*/true);
  const MetricsReport report = analyzer.analyze();
  ASSERT_EQ(report.tasks.size(), 2u);
  for (const auto& task : report.tasks) {
    EXPECT_TRUE(task.observed) << task.name;
    EXPECT_EQ(task.activations, 1) << task.name;  // one t=0 release, not per branch
    EXPECT_FALSE(task.wcrt.infinite) << task.name;
    EXPECT_EQ(task.wcrt.value, 6) << task.name;
  }
}

TEST(MetricsTest, StartReleaseInNonFirstInitialClassIsObserved) {
  // The initial active set has two classes. The first one does not carry the
  // task's entry token; only the second one does. Passing just the first
  // initial vertex (the historical behavior) therefore misses the release.
  petri::PTPN net = make_single_task_net(true);
  const size_t entry = place_index(net, "Tentry");
  const size_t ready = place_index(net, "Tready");
  const size_t exit = place_index(net, "Texit");
  const size_t done = place_index(net, "Tdone");
  const int get_core = static_cast<int>(transition_index(net, "Tget_core"));
  const int exec = static_cast<int>(transition_index(net, "Texec"));
  const int consume = static_cast<int>(transition_index(net, "Tconsume"));
  net.set_initial_marking(petri::PlaceId{entry}, 0);  // marking comes from the graph

  const std::vector<int> empty(net.num_places(), 0);
  std::vector<int> at_entry = empty;
  at_entry[entry] = 1;
  std::vector<int> at_ready = empty;
  at_ready[ready] = 1;
  std::vector<int> at_exit = empty;
  at_exit[exit] = 1;
  std::vector<int> at_done = empty;
  at_done[done] = 1;

  state_class::SCGraph graph;
  const state_class::SCVertex first = add_manual_state(graph, empty, 0);
  const state_class::SCVertex start = add_manual_state(graph, at_entry, 1);
  const state_class::SCVertex running = add_manual_state(graph, at_ready, 2);
  const state_class::SCVertex completed = add_manual_state(graph, at_exit, 3);
  const state_class::SCVertex drained = add_manual_state(graph, at_done, 4);
  add_manual_edge(graph, start, running, get_core, 0, 0);
  add_manual_edge(graph, running, completed, exec, 3, 5);
  add_manual_edge(graph, completed, drained, consume, 0, 0);

  MetricsAnalyzer analyzer(graph, net, std::vector<state_class::SCVertex>{first, start},
                           /*exact=*/true);
  const MetricsReport report = analyzer.analyze();
  ASSERT_EQ(report.tasks.size(), 1u);
  EXPECT_TRUE(report.tasks.front().observed);
  EXPECT_EQ(report.tasks.front().activations, 1);
  EXPECT_EQ(report.tasks.front().wcrt.value, 5);
  EXPECT_EQ(report.tasks.front().bcrt.value, 3);

  // Back-compat: the single-vertex overload still starts from one class only.
  MetricsAnalyzer first_only(graph, net, first, /*exact=*/true);
  const MetricsReport first_report = first_only.analyze();
  ASSERT_EQ(first_report.tasks.size(), 1u);
  EXPECT_FALSE(first_report.tasks.front().observed);
  EXPECT_EQ(first_report.tasks.front().activations, 0);
}

TEST(MetricsTest, ResponseTimeTakesMaximumOverInitialClasses) {
  // Two initial classes both carry the task's entry token but lead to
  // different execution windows. The response-time DP must start from both,
  // and the t=0 release is still a single activation.
  petri::PTPN net = make_single_task_net(true);
  const size_t entry = place_index(net, "Tentry");
  const size_t ready = place_index(net, "Tready");
  const size_t exit = place_index(net, "Texit");
  const size_t done = place_index(net, "Tdone");
  const int get_core = static_cast<int>(transition_index(net, "Tget_core"));
  const int exec = static_cast<int>(transition_index(net, "Texec"));
  const int consume = static_cast<int>(transition_index(net, "Tconsume"));
  net.set_initial_marking(petri::PlaceId{entry}, 0);

  const std::vector<int> empty(net.num_places(), 0);
  std::vector<int> at_entry = empty;
  at_entry[entry] = 1;
  std::vector<int> at_ready = empty;
  at_ready[ready] = 1;
  std::vector<int> at_exit = empty;
  at_exit[exit] = 1;
  std::vector<int> at_done = empty;
  at_done[done] = 1;

  state_class::SCGraph graph;
  const state_class::SCVertex short_start = add_manual_state(graph, at_entry, 0);
  const state_class::SCVertex short_ready = add_manual_state(graph, at_ready, 1);
  const state_class::SCVertex short_exit = add_manual_state(graph, at_exit, 2);
  const state_class::SCVertex short_done = add_manual_state(graph, at_done, 3);
  const state_class::SCVertex long_start = add_manual_state(graph, at_entry, 4);
  const state_class::SCVertex long_ready = add_manual_state(graph, at_ready, 5);
  const state_class::SCVertex long_exit = add_manual_state(graph, at_exit, 6);
  const state_class::SCVertex long_done = add_manual_state(graph, at_done, 7);
  add_manual_edge(graph, short_start, short_ready, get_core, 0, 0);
  add_manual_edge(graph, short_ready, short_exit, exec, 1, 1);
  add_manual_edge(graph, short_exit, short_done, consume, 0, 0);
  add_manual_edge(graph, long_start, long_ready, get_core, 0, 0);
  add_manual_edge(graph, long_ready, long_exit, exec, 9, 9);
  add_manual_edge(graph, long_exit, long_done, consume, 0, 0);

  MetricsAnalyzer analyzer(graph, net, std::vector<state_class::SCVertex>{short_start, long_start},
                           /*exact=*/true);
  const MetricsReport report = analyzer.analyze();
  ASSERT_EQ(report.tasks.size(), 1u);
  EXPECT_TRUE(report.tasks.front().observed);
  EXPECT_EQ(report.tasks.front().activations, 1);
  EXPECT_EQ(report.tasks.front().wcrt.value, 9);  // max over both initial classes

  // Starting from the first class alone reproduces the under-reporting bug.
  MetricsAnalyzer first_only(graph, net, short_start, /*exact=*/true);
  EXPECT_EQ(first_only.analyze().tasks.front().wcrt.value, 1);
}

}  // namespace
