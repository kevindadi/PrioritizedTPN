#include <boost/graph/adjacency_list.hpp>
#include <gtest/gtest.h>
#include <set>
#include <stdexcept>
#include <vector>

#include "analysis/dbm/clock_state.h"
#include "analysis/dbm/dbm.h"
#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "export/scg_dot.h"
#include "model/petri.h"

namespace {

using state_class::StateClass;
using state_class::StateClassReachabilityGraph;

// Collects the transition ids on the out-edges of a vertex.
std::multiset<int> out_edge_transitions(const state_class::SCGraph& graph,
                                        state_class::SCVertex vertex) {
  std::multiset<int> result;
  for (auto [it, end] = boost::out_edges(vertex, graph); it != end; ++it) {
    const auto& edge = boost::get(boost::edge_name, graph, *it);
    result.insert(edge.transition_id);
  }
  return result;
}

// Two independent control transitions that are both firable from the initial
// class; used to verify the analyzer branches over every firable transition.
petri::PTPN make_two_independent_transitions_net() {
  petri::PTPN ptpn;
  const petri::PlaceId left_in = ptpn.add_place("left_in", 1);
  const petri::PlaceId right_in = ptpn.add_place("right_in", 1);
  const petri::PlaceId left_done = ptpn.add_place("left_done", 1);
  const petri::PlaceId right_done = ptpn.add_place("right_done", 1);
  ptpn.set_initial_marking(left_in, 1);
  ptpn.set_initial_marking(right_in, 1);

  const petri::TransitionId left =
      ptpn.add_transition("left", petri::TimeInterval(0, 2), petri::INF, -1);
  const petri::TransitionId right =
      ptpn.add_transition("right", petri::TimeInterval(0, 2), petri::INF, -1);
  ptpn.set_pre_arc(left_in, left, 1);
  ptpn.set_post_arc(left, left_done, 1);
  ptpn.set_pre_arc(right_in, right, 1);
  ptpn.set_post_arc(right, right_done, 1);
  return ptpn;
}

// Low and high priority transitions on the same core competing for the CPU.
// Both are structurally enabled, but only the high-priority one is active.
petri::PTPN make_same_core_priority_net() {
  petri::PTPN ptpn;
  const petri::PlaceId input = ptpn.add_place("input", 2);
  ptpn.set_initial_marking(input, 1);

  // low: priority 1, suspendable; high: priority 9, not suspendable.
  const petri::TransitionId low = ptpn.add_transition("low", petri::TimeInterval(0, 5), 1, 0, true);
  const petri::TransitionId high =
      ptpn.add_transition("high", petri::TimeInterval(3, 3), 9, 0, false);
  ptpn.set_pre_arc(input, low, 1);
  ptpn.set_post_arc(low, input, 1);
  ptpn.set_pre_arc(input, high, 1);
  ptpn.set_post_arc(high, input, 1);
  return ptpn;
}

// A trigger that fires at time 2 alongside a survivor task that keeps running.
petri::PTPN make_persistent_survivor_net() {
  petri::PTPN ptpn;
  const petri::PlaceId trigger_in = ptpn.add_place("trigger_in", 1);
  const petri::PlaceId survivor_in = ptpn.add_place("survivor_in", 1);
  const petri::PlaceId trigger_done = ptpn.add_place("trigger_done", 1);
  const petri::PlaceId survivor_done = ptpn.add_place("survivor_done", 1);
  ptpn.set_initial_marking(trigger_in, 1);
  ptpn.set_initial_marking(survivor_in, 1);

  const petri::TransitionId trigger =
      ptpn.add_transition("trigger", petri::TimeInterval(2, 2), petri::INF, -1);
  const petri::TransitionId survivor =
      ptpn.add_transition("survivor", petri::TimeInterval(0, 5), petri::INF, -1);
  ptpn.set_pre_arc(trigger_in, trigger, 1);
  ptpn.set_post_arc(trigger, trigger_done, 1);
  ptpn.set_pre_arc(survivor_in, survivor, 1);
  ptpn.set_post_arc(survivor, survivor_done, 1);
  return ptpn;
}

// A preemptor on the same core that leaves once it fires, allowing the
// suspended low-priority task to resume.
petri::PTPN make_resume_net() {
  petri::PTPN ptpn;
  const petri::PlaceId low_in = ptpn.add_place("low_in", 1);
  const petri::PlaceId high_in = ptpn.add_place("high_in", 1);
  const petri::PlaceId low_done = ptpn.add_place("low_done", 1);
  const petri::PlaceId high_done = ptpn.add_place("high_done", 1);
  ptpn.set_initial_marking(low_in, 1);
  ptpn.set_initial_marking(high_in, 1);

  const petri::TransitionId low = ptpn.add_transition("low", petri::TimeInterval(0, 8), 1, 0, true);
  const petri::TransitionId high =
      ptpn.add_transition("high", petri::TimeInterval(0, 3), 9, 0, false);
  ptpn.set_pre_arc(low_in, low, 1);
  ptpn.set_post_arc(low, low_done, 1);
  ptpn.set_pre_arc(high_in, high, 1);
  ptpn.set_post_arc(high, high_done, 1);
  return ptpn;
}

// A low-priority suspendable execution segment preempted by a higher-priority
// segment on the same core. Task metadata is attached so the metrics analyzer
// reports per-task timings; consume transitions drain the exit tokens so the
// final class is a legitimate sink.
petri::PTPN make_preemption_metrics_net() {
  petri::PTPN ptpn;
  const petri::PlaceId low_entry = ptpn.add_place("Lentry", 1);
  const petri::PlaceId low_exit = ptpn.add_place("Lexit", 1);
  const petri::PlaceId low_done = ptpn.add_place("Ldone", 1);
  const petri::PlaceId high_entry = ptpn.add_place("Hentry", 1);
  const petri::PlaceId high_exit = ptpn.add_place("Hexit", 1);
  const petri::PlaceId high_done = ptpn.add_place("Hdone", 1);
  ptpn.set_initial_marking(low_entry, 1);
  ptpn.set_initial_marking(high_entry, 1);

  const petri::TransitionId low_exec =
      ptpn.add_transition("Lexec", petri::TimeInterval(0, 8), 1, 0, true);
  const petri::TransitionId high_exec =
      ptpn.add_transition("Hexec", petri::TimeInterval(0, 3), 9, 0, false);
  const petri::TransitionId low_consume =
      ptpn.add_transition("Lconsume", petri::TimeInterval(0, 0), 0, -1, false);
  const petri::TransitionId high_consume =
      ptpn.add_transition("Hconsume", petri::TimeInterval(0, 0), 0, -1, false);

  ptpn.set_pre_arc(low_entry, low_exec, 1);
  ptpn.set_post_arc(low_exec, low_exit, 1);
  ptpn.set_pre_arc(low_exit, low_consume, 1);
  ptpn.set_post_arc(low_consume, low_done, 1);
  ptpn.set_pre_arc(high_entry, high_exec, 1);
  ptpn.set_post_arc(high_exec, high_exit, 1);
  ptpn.set_pre_arc(high_exit, high_consume, 1);
  ptpn.set_post_arc(high_consume, high_done, 1);

  ptpn.set_task_chain("L", {petri::NodeRef::of(low_entry), petri::NodeRef::of(low_exec),
                            petri::NodeRef::of(low_exit)});
  ptpn.set_node_span("L", petri::NodeRef::of(low_entry), petri::NodeRef::of(low_exit));
  ptpn.set_task_chain("H", {petri::NodeRef::of(high_entry), petri::NodeRef::of(high_exec),
                            petri::NodeRef::of(high_exit)});
  ptpn.set_node_span("H", petri::NodeRef::of(high_entry), petri::NodeRef::of(high_exit));

  petri::TaskInfo low_info;
  low_info.core = 0;
  low_info.priority = 1;
  low_info.wcet = 8;
  low_info.bcet = 0;
  ptpn.set_task_info("L", low_info);

  petri::TaskInfo high_info;
  high_info.core = 0;
  high_info.priority = 9;
  high_info.wcet = 3;
  high_info.bcet = 0;
  ptpn.set_task_info("H", high_info);
  return ptpn;
}

// A trigger that, when fired, enables two sibling transitions at once.
petri::PTPN make_newly_enabled_siblings_net() {
  petri::PTPN ptpn;
  const petri::PlaceId input = ptpn.add_place("input", 1);
  const petri::PlaceId shared = ptpn.add_place("shared", 2);
  const petri::PlaceId left_done = ptpn.add_place("left_done", 1);
  const petri::PlaceId right_done = ptpn.add_place("right_done", 1);
  ptpn.set_initial_marking(input, 1);

  const petri::TransitionId trigger =
      ptpn.add_transition("trigger", petri::TimeInterval(0, 0), petri::INF, -1);
  const petri::TransitionId left =
      ptpn.add_transition("left", petri::TimeInterval(0, 4), petri::INF, -1);
  const petri::TransitionId right =
      ptpn.add_transition("right", petri::TimeInterval(0, 6), petri::INF, -1);
  ptpn.set_pre_arc(input, trigger, 1);
  ptpn.set_post_arc(trigger, shared, 2);
  ptpn.set_pre_arc(shared, left, 1);
  ptpn.set_post_arc(left, left_done, 1);
  ptpn.set_pre_arc(shared, right, 1);
  ptpn.set_post_arc(right, right_done, 1);
  return ptpn;
}

}  // namespace

// --- DBM unit tests (ported) -----------------------------------------------

TEST(DbmTest, FutureRemovesOnlyUnfrozenLowerBounds) {
  state_class::DBM dbm(3);
  dbm.set_constraint(0, 1, -2);
  dbm.set_constraint(0, 2, -4);
  dbm.freeze_clock(2);

  state_class::reset_dbm_instrumentation();
  dbm.future();

  EXPECT_EQ(dbm.get_constraint(0, 1), state_class::INF_TIME);
  EXPECT_EQ(dbm.get_constraint(0, 2), -4);
  EXPECT_EQ(state_class::get_dbm_instrumentation().minimize_calls, 1u);
}

TEST(DbmTest, ConstrainUpperBoundOnlyTightensFiniteBounds) {
  state_class::DBM dbm(2);
  dbm.set_constraint(1, 0, 9);

  dbm.constrain_upper_bound(1, 7);
  EXPECT_EQ(dbm.get_constraint(1, 0), 7);
  dbm.constrain_upper_bound(1, 8);
  EXPECT_EQ(dbm.get_constraint(1, 0), 7);
  dbm.constrain_upper_bound(1, state_class::INF_TIME);
  EXPECT_EQ(dbm.get_constraint(1, 0), 7);
}

TEST(DbmTest, SynchronizeClocksForcesPairwiseEquality) {
  state_class::DBM dbm(3);
  dbm.set_constraint(0, 1, -2);
  dbm.set_constraint(1, 0, 5);
  dbm.set_constraint(0, 2, -4);
  dbm.set_constraint(2, 0, 7);

  dbm.synchronize_clocks({1, 2});

  EXPECT_EQ(dbm.get_constraint(1, 2), 0);
  EXPECT_EQ(dbm.get_constraint(2, 1), 0);
}

TEST(DbmTest, IncludedInDetectsZoneSubset) {
  state_class::DBM tight(2);
  tight.set_constraint(0, 1, -2);
  tight.set_constraint(1, 0, 4);
  tight.minimize();

  state_class::DBM loose(2);
  loose.set_constraint(0, 1, -1);
  loose.set_constraint(1, 0, 6);
  loose.minimize();

  EXPECT_TRUE(tight.included_in(loose));
  EXPECT_FALSE(loose.included_in(tight));
}

// --- State-class construction tests ----------------------------------------

TEST(PtpnAnalysisTest, InitialClassPinsEveryClockToZero) {
  const petri::PTPN ptpn = make_newly_enabled_siblings_net();
  StateClassReachabilityGraph graph(ptpn);
  const StateClass initial = graph.compute_initial_class();

  ASSERT_TRUE(initial.has_exec_clock(0));
  const size_t idx = static_cast<size_t>(initial.exec_index(0));
  EXPECT_EQ(initial.zone.get_constraint(0, idx), 0);
  EXPECT_EQ(initial.zone.get_constraint(idx, 0), 0);
}

TEST(PtpnAnalysisTest, BranchesOverEveryFirableTransition) {
  const petri::PTPN ptpn = make_two_independent_transitions_net();
  StateClassReachabilityGraph graph(ptpn);
  graph.build(64);

  const auto transitions = out_edge_transitions(graph.get_graph(), graph.get_initial_vertex());
  EXPECT_EQ(transitions.count(0), 1u);
  EXPECT_EQ(transitions.count(1), 1u);
}

TEST(PtpnAnalysisTest, PriorityFilterFiresHighPriorityNotEarliest) {
  // low (T0) has the earliest window [0,5] but lower priority; high (T1) is
  // [3,3]. Under the priority semantics only the high-priority transition is
  // active, so the analyzer must fire T1 rather than the earlier T0.
  const petri::PTPN ptpn = make_same_core_priority_net();
  StateClassReachabilityGraph graph(ptpn);
  graph.build(64);

  const StateClass initial =
      boost::get(boost::vertex_name, graph.get_graph(), graph.get_initial_vertex());
  EXPECT_EQ(initial.priority_enabled, (state_class::TransitionSet{1}));
  EXPECT_EQ(initial.suspended, (state_class::TransitionSet{0}));

  const auto transitions = out_edge_transitions(graph.get_graph(), graph.get_initial_vertex());
  EXPECT_EQ(transitions.count(1), 1u);
  EXPECT_EQ(transitions.count(0), 0u);
}

TEST(PtpnAnalysisTest, ControlTransitionsAreAlsoPriorityFiltered) {
  // Two control transitions (core -1) of different priority, both enabled.
  // The control core is now a normal core group, so only the higher-priority
  // one is active (this is what lets a resume transition win over ordinary
  // control steps).
  petri::PTPN ptpn;
  const petri::PlaceId low_in = ptpn.add_place("low_in", 1);
  const petri::PlaceId high_in = ptpn.add_place("high_in", 1);
  const petri::PlaceId low_done = ptpn.add_place("low_done", 1);
  const petri::PlaceId high_done = ptpn.add_place("high_done", 1);
  ptpn.set_initial_marking(low_in, 1);
  ptpn.set_initial_marking(high_in, 1);

  const petri::TransitionId low_ctrl =
      ptpn.add_transition("low_ctrl", petri::TimeInterval(0, 0), 0, -1);
  const petri::TransitionId high_ctrl =
      ptpn.add_transition("high_ctrl", petri::TimeInterval(0, 0), 1, -1);
  ptpn.set_pre_arc(low_in, low_ctrl, 1);
  ptpn.set_post_arc(low_ctrl, low_done, 1);
  ptpn.set_pre_arc(high_in, high_ctrl, 1);
  ptpn.set_post_arc(high_ctrl, high_done, 1);

  StateClassReachabilityGraph graph(ptpn);
  const StateClass initial = graph.compute_initial_class();
  EXPECT_EQ(initial.priority_enabled, (state_class::TransitionSet{1}));
}

// Builds two equal-priority execution transitions on the same real core (0).
petri::PTPN make_same_core_equal_priority_net() {
  petri::PTPN ptpn;
  const petri::PlaceId a_in = ptpn.add_place("a_in", 1);
  const petri::PlaceId b_in = ptpn.add_place("b_in", 1);
  const petri::PlaceId a_done = ptpn.add_place("a_done", 1);
  const petri::PlaceId b_done = ptpn.add_place("b_done", 1);
  ptpn.set_initial_marking(a_in, 1);
  ptpn.set_initial_marking(b_in, 1);

  const petri::TransitionId a_exec =
      ptpn.add_transition("a_exec", petri::TimeInterval(1, 2), 5, 0, true);
  const petri::TransitionId b_exec =
      ptpn.add_transition("b_exec", petri::TimeInterval(1, 2), 5, 0, true);
  ptpn.set_pre_arc(a_in, a_exec, 1);
  ptpn.set_post_arc(a_exec, a_done, 1);
  ptpn.set_pre_arc(b_in, b_exec, 1);
  ptpn.set_post_arc(b_exec, b_done, 1);
  return ptpn;
}

TEST(PtpnAnalysisTest, CoreCapacityOneEnumeratesEveryTieResolution) {
  // Equal priority on the same core with one slot: both transitions are
  // possible scheduling choices, so the semantics must keep both active sets.
  // Their union is the set of all possible transitions; there is no
  // deterministic "smallest index wins" shortcut.
  petri::PTPN ptpn = make_same_core_equal_priority_net();
  ptpn.set_core_parallelism(0, 1);

  StateClassReachabilityGraph graph(ptpn);
  const std::vector<StateClass> initials = graph.compute_initial_classes();

  ASSERT_EQ(initials.size(), 2u);
  std::set<state_class::TransitionSet> branches;
  for (const StateClass& initial : initials) {
    branches.insert(initial.priority_enabled);
    EXPECT_EQ(initial.suspended.size(), 1u);
    EXPECT_EQ(initial.priority_enabled.size() + initial.suspended.size(),
              initial.struct_enabled.size());
  }
  EXPECT_EQ(branches.count(state_class::TransitionSet{0}), 1u);
  EXPECT_EQ(branches.count(state_class::TransitionSet{1}), 1u);

  // A singular query must refuse to pick one branch silently.
  EXPECT_THROW(graph.compute_initial_class(), std::runtime_error);
}

TEST(PtpnAnalysisTest, CoreCapacityTwoAllowsParallelExecution) {
  // The same core declared with two slots lets both equal-priority tasks run
  // in parallel, so there is a single active set containing both transitions.
  petri::PTPN ptpn = make_same_core_equal_priority_net();
  ptpn.set_core_parallelism(0, 2);

  StateClassReachabilityGraph graph(ptpn);
  const StateClass initial = graph.compute_initial_class();
  EXPECT_EQ(initial.priority_enabled, (state_class::TransitionSet{0, 1}));
  EXPECT_TRUE(initial.suspended.empty());
}

TEST(PtpnAnalysisTest, CoreCapacityTwoFillsSecondSlotWithLowerPriority) {
  // With two slots, the highest-priority transition takes one and the next
  // priority fills the second; only ties at the cutoff create branches.
  petri::PTPN ptpn = make_same_core_priority_net();
  ptpn.set_core_parallelism(0, 2);

  StateClassReachabilityGraph graph(ptpn);
  const StateClass initial = graph.compute_initial_class();
  EXPECT_EQ(initial.priority_enabled, (state_class::TransitionSet{0, 1}));
  EXPECT_TRUE(initial.suspended.empty());
}

TEST(PtpnAnalysisTest, BuildKeepsEveryInitialTieResolution) {
  petri::PTPN ptpn = make_same_core_equal_priority_net();
  ptpn.set_core_parallelism(0, 1);

  StateClassReachabilityGraph graph(ptpn);
  graph.build(256);
  EXPECT_GE(graph.get_statistics().total_states, 2u);
}

TEST(PtpnAnalysisTest, SuspendedTransitionFreezesExecAndRunsSuspensionClock) {
  const petri::PTPN ptpn = make_same_core_priority_net();
  StateClassReachabilityGraph graph(ptpn);
  graph.set_suspension_clock_projection(false);  // inspect the w_t clock directly
  const StateClass initial = graph.compute_initial_class();
  const StateClass elapsed = graph.time_elapse(initial);

  ASSERT_TRUE(elapsed.has_exec_clock(0));  // low has a frozen exec clock
  ASSERT_TRUE(elapsed.has_susp_clock(0));  // low has a running suspension clock
  ASSERT_TRUE(elapsed.has_exec_clock(1));  // high has a running exec clock

  const size_t low_exec = static_cast<size_t>(elapsed.exec_index(0));
  const size_t low_susp = static_cast<size_t>(elapsed.susp_index(0));
  const size_t high_exec = static_cast<size_t>(elapsed.exec_index(1));

  // low's execution clock stays frozen at 0 while it is suspended.
  EXPECT_EQ(elapsed.zone.get_constraint(low_exec, 0), 0);
  // high's execution clock is capped at its deadline 3 (strong time).
  EXPECT_EQ(elapsed.zone.get_constraint(high_exec, 0), 3);
  // low's suspension clock advances together with time, up to high's deadline.
  EXPECT_EQ(elapsed.zone.get_constraint(low_susp, 0), 3);
}

TEST(PtpnAnalysisTest, PersistentTransitionKeepsAccumulatedClock) {
  const petri::PTPN ptpn = make_persistent_survivor_net();
  StateClassReachabilityGraph graph(ptpn);
  const StateClass initial = graph.compute_initial_class();
  const StateClass elapsed = graph.time_elapse(initial);

  StateClass successor;
  ASSERT_TRUE(graph.fire(elapsed, 0, successor));  // fire the trigger at t=2

  ASSERT_TRUE(state_class::contains(successor.struct_enabled, 1));  // survivor still enabled
  ASSERT_TRUE(successor.has_exec_clock(1));
  const size_t survivor = static_cast<size_t>(successor.exec_index(1));
  // The survivor's elapsed time (2) is preserved, not reset to 0.
  EXPECT_EQ(successor.zone.get_constraint(0, survivor), -2);
}

TEST(PtpnAnalysisTest, ResumedTransitionDropsSuspensionKeepsExec) {
  const petri::PTPN ptpn = make_resume_net();
  StateClassReachabilityGraph graph(ptpn);
  graph.set_suspension_clock_projection(false);  // exercise w_t inheritance/drop
  const StateClass initial = graph.compute_initial_class();

  ASSERT_EQ(initial.priority_enabled, (state_class::TransitionSet{1}));  // high active
  ASSERT_EQ(initial.suspended, (state_class::TransitionSet{0}));         // low suspended

  const StateClass elapsed = graph.time_elapse(initial);
  StateClass successor;
  ASSERT_TRUE(graph.fire(elapsed, 1, successor));  // high fires and leaves

  EXPECT_EQ(successor.priority_enabled, (state_class::TransitionSet{0}));  // low resumed
  EXPECT_TRUE(successor.suspended.empty());
  EXPECT_TRUE(successor.has_exec_clock(0));
  EXPECT_FALSE(successor.has_susp_clock(0));  // suspension clock dropped
}

namespace {

void expect_time_value_equal(const state_class::TimeValue& projected,
                             const state_class::TimeValue& kept, const std::string& label) {
  EXPECT_EQ(projected.infinite, kept.infinite) << label;
  if (!projected.infinite && !kept.infinite) {
    EXPECT_EQ(projected.value, kept.value) << label;
  }
}

}  // namespace

TEST(PtpnAnalysisTest, SuspensionClockProjectionPreservesMetrics) {
  const petri::PTPN ptpn = make_preemption_metrics_net();

  StateClassReachabilityGraph projected(ptpn);
  ASSERT_TRUE(projected.get_suspension_clock_projection());  // default
  projected.build(1000);

  StateClassReachabilityGraph kept(ptpn);
  kept.set_suspension_clock_projection(false);
  kept.build(1000);

  ASSERT_FALSE(projected.get_statistics().truncated);
  ASSERT_FALSE(kept.get_statistics().truncated);
  EXPECT_LE(projected.get_statistics().total_states, kept.get_statistics().total_states);

  // The suspended set survives the projection; only the w_t columns disappear.
  const StateClass& projected_initial =
      boost::get(boost::vertex_name, projected.get_graph(), projected.get_initial_vertex());
  const StateClass& kept_initial =
      boost::get(boost::vertex_name, kept.get_graph(), kept.get_initial_vertex());
  EXPECT_EQ(projected_initial.suspended, (state_class::TransitionSet{0}));
  EXPECT_EQ(kept_initial.suspended, (state_class::TransitionSet{0}));
  EXPECT_FALSE(projected_initial.has_susp_clock(0));
  EXPECT_TRUE(kept_initial.has_susp_clock(0));
  for (const state_class::ClockVar& var : projected_initial.clock_vars) {
    EXPECT_NE(var.kind, state_class::ClockKind::Suspension);
  }

  state_class::MetricsAnalyzer projected_analyzer(projected.get_graph(), ptpn,
                                                  projected.get_initial_vertex(),
                                                  /*exact=*/true);
  const state_class::MetricsReport p = projected_analyzer.analyze();
  state_class::MetricsAnalyzer kept_analyzer(kept.get_graph(), ptpn, kept.get_initial_vertex(),
                                             /*exact=*/true);
  const state_class::MetricsReport k = kept_analyzer.analyze();

  EXPECT_LE(p.states, k.states);
  EXPECT_EQ(p.bounded, k.bounded);
  EXPECT_EQ(p.schedulable, k.schedulable);
  EXPECT_EQ(p.deadlock_states.size(), k.deadlock_states.size());
  ASSERT_EQ(p.tasks.size(), k.tasks.size());
  ASSERT_EQ(p.tasks.size(), 2u);

  for (size_t i = 0; i < p.tasks.size(); ++i) {
    const state_class::TaskMetrics& a = p.tasks[i];
    const state_class::TaskMetrics& b = k.tasks[i];
    SCOPED_TRACE(a.name);
    EXPECT_EQ(a.core, b.core);
    EXPECT_EQ(a.priority, b.priority);
    EXPECT_EQ(a.wcet, b.wcet);
    EXPECT_EQ(a.bcet, b.bcet);
    EXPECT_EQ(a.period, b.period);
    EXPECT_EQ(a.deadline, b.deadline);
    EXPECT_EQ(a.observed, b.observed);
    EXPECT_EQ(a.activations, b.activations);
    // WCRT upper and certified lower bounds, and every derived timing value,
    // must agree between the projected and unprojected encodings.
    expect_time_value_equal(a.wcrt, b.wcrt, "wcrt");
    expect_time_value_equal(a.wcrt_lb, b.wcrt_lb, "wcrt_lb");
    EXPECT_EQ(a.wcrt_interval_tight, b.wcrt_interval_tight);
    expect_time_value_equal(a.bcrt, b.bcrt, "bcrt");
    expect_time_value_equal(a.jitter, b.jitter, "jitter");
    expect_time_value_equal(a.worst_interference, b.worst_interference, "worst_interference");
    expect_time_value_equal(a.worst_blocking, b.worst_blocking, "worst_blocking");
    expect_time_value_equal(a.slack, b.slack, "slack");
    EXPECT_EQ(a.deadline_missed, b.deadline_missed);
    EXPECT_EQ(a.local_deadlock, b.local_deadlock);
    EXPECT_EQ(a.local_deadlock_scc_size, b.local_deadlock_scc_size);
    EXPECT_EQ(a.max_preemptions, b.max_preemptions);
    EXPECT_EQ(a.max_in_flight, b.max_in_flight);
    EXPECT_EQ(a.jobs_per_hyperperiod, b.jobs_per_hyperperiod);
  }
}

TEST(PtpnAnalysisTest, NewlyEnabledTransitionsResetToZero) {
  const petri::PTPN ptpn = make_newly_enabled_siblings_net();
  StateClassReachabilityGraph graph(ptpn);
  const StateClass initial = graph.compute_initial_class();
  const StateClass elapsed = graph.time_elapse(initial);

  StateClass successor;
  ASSERT_TRUE(graph.fire(elapsed, 0, successor));  // fire the trigger

  ASSERT_TRUE(successor.has_exec_clock(1));
  ASSERT_TRUE(successor.has_exec_clock(2));
  const size_t left = static_cast<size_t>(successor.exec_index(1));
  const size_t right = static_cast<size_t>(successor.exec_index(2));
  EXPECT_EQ(successor.zone.get_constraint(0, left), 0);
  EXPECT_EQ(successor.zone.get_constraint(0, right), 0);
}

TEST(PtpnAnalysisTest, BuildTerminatesAndCountsStates) {
  const petri::PTPN ptpn = make_persistent_survivor_net();
  StateClassReachabilityGraph graph(ptpn);
  const size_t states = graph.build(64);

  EXPECT_GE(states, 1u);
  EXPECT_FALSE(graph.get_statistics().truncated);
  EXPECT_EQ(states, graph.get_statistics().total_states);
}

TEST(PtpnAnalysisTest, NamedDumpIncludesPlaceAndClockLabels) {
  const petri::PTPN ptpn = make_same_core_priority_net();
  StateClassReachabilityGraph graph(ptpn);
  const StateClass initial = graph.compute_initial_class();
  const scg_export::ScgFormatter formatter(ptpn, graph.get_graph());

  const std::string dump = formatter.format_state_dump(initial);
  EXPECT_NE(dump.find("input"), std::string::npos);
  EXPECT_NE(dump.find("E_pri"), std::string::npos);

  const std::string zone = formatter.format_named_dbm(initial);
  EXPECT_NE(zone.find("h(T1)"), std::string::npos);
}
