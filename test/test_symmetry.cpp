#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"
#include "analysis/reachability/state_class.h"
#include "model/petri.h"

namespace {

using state_class::MetricsAnalyzer;
using state_class::MetricsReport;
using state_class::StateClass;
using state_class::StateClassReachabilityGraph;
using state_class::TaskMetrics;

struct TaskSpec {
  std::string name;
  int core = 0;
  int priority = 1;
  int earliest = 3;
  int latest = 5;
  std::vector<std::string> locks;
};

// One TDG-style task chain:
//   entry -(get_core,[0,0])-> ready -(exec,[e,l])-> exit -(consume)-> done
// The consume transition drains the exit token so the terminal class carries no
// chain work; compute transitions run on the control core.
void add_task(petri::PTPN& net, const TaskSpec& spec) {
  const petri::PlaceId entry = net.add_place(spec.name + "_entry", 1);
  const petri::TransitionId get_core =
      net.add_transition(spec.name + "_get_core", petri::TimeInterval(0, 0), spec.priority,
                         spec.core, false);
  const petri::PlaceId ready = net.add_place(spec.name + "_ready", 1);
  const petri::TransitionId exec = net.add_transition(
      spec.name + "_exec", petri::TimeInterval(spec.earliest, spec.latest), spec.priority,
      spec.core, false);
  const petri::PlaceId exit = net.add_place(spec.name + "_exit", 1);
  const petri::TransitionId consume =
      net.add_transition(spec.name + "_consume", petri::TimeInterval(0, 0), 0, -1, false);
  const petri::PlaceId done = net.add_place(spec.name + "_done", 1);

  net.set_initial_marking(entry, 1);
  net.set_pre_arc(entry, get_core, 1);
  net.set_post_arc(get_core, ready, 1);
  net.set_pre_arc(ready, exec, 1);
  net.set_post_arc(exec, exit, 1);
  net.set_pre_arc(exit, consume, 1);
  net.set_post_arc(consume, done, 1);

  net.set_task_chain(
      spec.name,
      {petri::NodeRef::of(entry), petri::NodeRef::of(get_core), petri::NodeRef::of(ready),
       petri::NodeRef::of(exec), petri::NodeRef::of(exit)});
  net.set_node_span(spec.name, petri::NodeRef::of(entry), petri::NodeRef::of(exit));

  petri::TaskInfo info;
  info.core = spec.core;
  info.priority = spec.priority;
  info.wcet = spec.latest;
  info.bcet = spec.earliest;
  info.period = 0;
  info.deadline = 0;
  info.locks = spec.locks;
  net.set_task_info(spec.name, info);
}

petri::PTPN make_net(const std::vector<TaskSpec>& specs) {
  petri::PTPN net;
  for (const TaskSpec& spec : specs) {
    add_task(net, spec);
  }
  return net;
}

// Owns the graph so callers can run metrics on it.
class SymmetryRun {
 public:
  SymmetryRun(const petri::PTPN& net, bool symmetry) : graph_(net) {
    graph_.set_symmetry_reduction(symmetry);
    graph_.build(100000);
  }

  [[nodiscard]] size_t states() const {
    return graph_.get_statistics().total_states;
  }
  [[nodiscard]] size_t edges() const {
    return graph_.get_statistics().total_transitions;
  }
  [[nodiscard]] bool applied() const {
    return graph_.symmetry_applied();
  }
  [[nodiscard]] size_t group_order() const {
    return graph_.symmetry_group_order();
  }
  [[nodiscard]] const std::vector<std::vector<std::string>>& groups() const {
    return graph_.symmetry_task_groups();
  }
  [[nodiscard]] const state_class::SCGraph& graph() const {
    return graph_.get_graph();
  }
  [[nodiscard]] state_class::SCVertex initial() const {
    return graph_.get_initial_vertex();
  }

 private:
  StateClassReachabilityGraph graph_;
};

MetricsReport analyze(const petri::PTPN& net, bool symmetry) {
  SymmetryRun run(net, symmetry);
  MetricsAnalyzer analyzer(run.graph(), net, run.initial(), /*exact=*/true);
  analyzer.set_symmetry_task_groups(run.groups());
  return analyzer.analyze();
}

const TaskMetrics* find_task(const MetricsReport& report, const std::string& name) {
  for (const TaskMetrics& task : report.tasks) {
    if (task.name == name) {
      return &task;
    }
  }
  return nullptr;
}

// Two structurally identical tasks on two distinct real cores, no shared locks
// and no dependency edges: the net is symmetric under swapping the tasks.
petri::PTPN make_symmetric_pair() {
  return make_net({
      {"T1", 0, 1, 3, 5, {}},
      {"T2", 1, 1, 3, 5, {}},
  });
}

// Canonical states must still satisfy the state-class invariants: the enabled
// set is exactly the structural enabling of the marking and the clock layout
// matches the derived indices.
void expect_canonical_invariants(const petri::PTPN& net, const state_class::SCGraph& graph) {
  for (auto [it, end] = boost::vertices(graph); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, graph, *it);
    const auto expected =
        state_class::Scheduling::structural_enabled(net, state.marking);
    ASSERT_EQ(state.struct_enabled, expected);

    StateClass rebuilt = state;
    state_class::build_clock_layout(rebuilt, net.num_transitions());
    ASSERT_EQ(rebuilt.clock_vars.size(), state.clock_vars.size());
    for (size_t i = 0; i < rebuilt.clock_vars.size(); ++i) {
      ASSERT_EQ(rebuilt.clock_vars[i].kind, state.clock_vars[i].kind);
      ASSERT_EQ(rebuilt.clock_vars[i].transition, state.clock_vars[i].transition);
    }
    ASSERT_EQ(state.zone.size(), state.clock_vars.size());
  }
}

TEST(SymmetryTest, IdenticalTasksOnDistinctCoresAreQuotiented) {
  const petri::PTPN net = make_symmetric_pair();

  SymmetryRun off(net, false);
  SymmetryRun on(net, true);

  EXPECT_FALSE(off.applied());
  EXPECT_TRUE(on.applied());
  EXPECT_EQ(on.group_order(), 2u);
  ASSERT_EQ(on.groups().size(), 1u);
  EXPECT_EQ(on.groups().front(), (std::vector<std::string>{"T1", "T2"}));

  // The two interleavings collapse: the quotient must be strictly smaller.
  EXPECT_LT(on.states(), off.states());
  EXPECT_LT(on.edges(), off.edges());
  expect_canonical_invariants(net, on.graph());

  const MetricsReport report_off = analyze(net, false);
  const MetricsReport report_on = analyze(net, true);
  const TaskMetrics* off1 = find_task(report_off, "T1");
  const TaskMetrics* off2 = find_task(report_off, "T2");
  const TaskMetrics* on1 = find_task(report_on, "T1");
  const TaskMetrics* on2 = find_task(report_on, "T2");
  ASSERT_NE(off1, nullptr);
  ASSERT_NE(off2, nullptr);
  ASSERT_NE(on1, nullptr);
  ASSERT_NE(on2, nullptr);

  ASSERT_TRUE(off1->observed);
  ASSERT_TRUE(off2->observed);
  EXPECT_TRUE(on1->observed);
  EXPECT_TRUE(on2->observed);

  // Both tasks are symmetric, so their values coincide; the quotient must
  // reproduce the unreduced per-task WCRT and certified lower bound.
  EXPECT_EQ(on1->wcrt.value, off1->wcrt.value);
  EXPECT_EQ(on2->wcrt.value, off2->wcrt.value);
  EXPECT_EQ(on1->wcrt_lb.value, off1->wcrt_lb.value);
  EXPECT_EQ(on2->wcrt_lb.value, off2->wcrt_lb.value);
  EXPECT_EQ(off1->wcrt_lb.value, 3);
  EXPECT_EQ(off2->wcrt_lb.value, 3);
  EXPECT_EQ(on1->bcrt.value, off1->bcrt.value);
  EXPECT_EQ(on2->bcrt.value, off2->bcrt.value);
}

TEST(SymmetryTest, HeterogeneousTasksAreNotMerged) {
  // Same wiring and cores, different execution windows: not isomorphic.
  const petri::PTPN net = make_net({
      {"T1", 0, 1, 3, 5, {}},
      {"T2", 1, 1, 2, 4, {}},
  });

  SymmetryRun off(net, false);
  SymmetryRun on(net, true);

  EXPECT_FALSE(on.applied());
  EXPECT_EQ(on.group_order(), 1u);
  EXPECT_TRUE(on.groups().empty());
  EXPECT_EQ(on.states(), off.states());
  EXPECT_EQ(on.edges(), off.edges());

  const MetricsReport report_off = analyze(net, false);
  const MetricsReport report_on = analyze(net, true);
  const TaskMetrics* off1 = find_task(report_off, "T1");
  const TaskMetrics* off2 = find_task(report_off, "T2");
  const TaskMetrics* on1 = find_task(report_on, "T1");
  const TaskMetrics* on2 = find_task(report_on, "T2");
  ASSERT_NE(off1, nullptr);
  ASSERT_NE(off2, nullptr);
  ASSERT_NE(on1, nullptr);
  ASSERT_NE(on2, nullptr);
  EXPECT_EQ(on1->wcrt.value, off1->wcrt.value);
  EXPECT_EQ(on2->wcrt.value, off2->wcrt.value);
  EXPECT_NE(off1->wcrt.value, off2->wcrt.value);
}

TEST(SymmetryTest, NonIsomorphicLockUserBlocksGrouping) {
  // T1/T2 would be interchangeable (same lock, same timing), but T3 is a
  // non-isomorphic task that also uses lock L. The conservative lock rule must
  // drop the whole group rather than merge across a shared resource.
  const petri::PTPN net = make_net({
      {"T1", 0, 1, 3, 5, {"L"}},
      {"T2", 1, 1, 3, 5, {"L"}},
      {"T3", 2, 1, 1, 2, {"L"}},
  });

  SymmetryRun off(net, false);
  SymmetryRun on(net, true);

  EXPECT_FALSE(on.applied());
  EXPECT_EQ(on.group_order(), 1u);
  EXPECT_TRUE(on.groups().empty());
  EXPECT_EQ(on.states(), off.states());
  EXPECT_EQ(on.edges(), off.edges());
}

TEST(SymmetryTest, DistinctIsolatedLockGroupsAreQuotiented) {
  // Two independent symmetric pairs on four cores; each pair owns its lock.
  const petri::PTPN net = make_net({
      {"A1", 0, 1, 3, 5, {"LA"}},
      {"A2", 1, 1, 3, 5, {"LA"}},
      {"B1", 2, 1, 1, 2, {"LB"}},
      {"B2", 3, 1, 1, 2, {"LB"}},
  });

  SymmetryRun off(net, false);
  SymmetryRun on(net, true);

  EXPECT_TRUE(on.applied());
  EXPECT_EQ(on.groups().size(), 2u);
  // The verifier proves each pair interchangeable; the generated subgroup may
  // consist only of the simultaneous double swap (a sound partial quotient).
  EXPECT_GE(on.group_order(), 2u);
  EXPECT_LT(on.states(), off.states());
  EXPECT_LT(on.edges(), off.edges());

  const MetricsReport report_off = analyze(net, false);
  const MetricsReport report_on = analyze(net, true);
  for (const std::string& name : {"A1", "A2", "B1", "B2"}) {
    const TaskMetrics* off_task = find_task(report_off, name);
    const TaskMetrics* on_task = find_task(report_on, name);
    ASSERT_NE(off_task, nullptr);
    ASSERT_NE(on_task, nullptr);
    EXPECT_EQ(on_task->wcrt.value, off_task->wcrt.value) << name;
    EXPECT_EQ(on_task->wcrt_lb.value, off_task->wcrt_lb.value) << name;
  }
}

TEST(SymmetryTest, ReductionIsOffByDefault) {
  const petri::PTPN net = make_symmetric_pair();
  StateClassReachabilityGraph graph(net);
  EXPECT_FALSE(graph.get_symmetry_reduction());
  graph.build(100000);
  EXPECT_FALSE(graph.symmetry_applied());
}

}  // namespace
