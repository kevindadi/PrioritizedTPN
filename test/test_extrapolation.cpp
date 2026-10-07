#include <boost/graph/adjacency_list.hpp>
#include <gtest/gtest.h>
#include <set>
#include <vector>

#include "analysis/clock_state.h"
#include "analysis/dbm.h"
#include "analysis/ptpn_analysis.h"
#include "model/petri.h"

namespace {

using state_class::DBM;
using state_class::StateClassReachabilityGraph;

// Collects the set of distinct markings stored in the reachability graph.
std::set<std::vector<int>> reachable_markings(const state_class::SCGraph& graph) {
  std::set<std::vector<int>> markings;
  for (auto [it, end] = boost::vertices(graph); it != end; ++it) {
    markings.insert(boost::get(boost::vertex_name, graph, *it).marking);
  }
  return markings;
}

// A periodic ticker t_tick[1,1] runs forever next to a transition t_slow with
// an unbounded interval [5, inf). t_slow stays enabled without being forced to
// fire, so its execution clock h_slow grows by one with every tick. Without
// extrapolation every tick creates a fresh state class (h_slow = 0, 1, 2, ...);
// with k-extrapolation the classes with h_slow >= k merge and the graph is
// finite.
petri::PTPN make_unbounded_clock_net() {
  petri::PTPN ptpn;
  const petri::PlaceId tick_p = ptpn.add_place("tick_p", 1);
  const petri::PlaceId slow_p = ptpn.add_place("slow_p", 1);
  const petri::PlaceId slow_done = ptpn.add_place("slow_done", 1);
  ptpn.set_initial_marking(tick_p, 1);
  ptpn.set_initial_marking(slow_p, 1);

  const petri::TransitionId tick =
      ptpn.add_transition("tick", petri::TimeInterval(1, 1), petri::INF, -1);
  const petri::TransitionId slow =
      ptpn.add_transition("slow", petri::TimeInterval(5, petri::INF), petri::INF, -1);
  ptpn.set_pre_arc(tick_p, tick, 1);
  ptpn.set_post_arc(tick, tick_p, 1);
  ptpn.set_pre_arc(slow_p, slow, 1);
  ptpn.set_post_arc(slow, slow_done, 1);
  return ptpn;
}

// Same-core low/high priority pair where the low-priority transition gets
// suspended: exercises frozen execution clocks and suspension clocks.
petri::PTPN make_preemption_net() {
  petri::PTPN ptpn;
  const petri::PlaceId low_in = ptpn.add_place("low_in", 1);
  const petri::PlaceId low_done = ptpn.add_place("low_done", 1);
  const petri::PlaceId high_in = ptpn.add_place("high_in", 1);
  const petri::PlaceId high_done = ptpn.add_place("high_done", 1);
  ptpn.set_initial_marking(low_in, 1);
  ptpn.set_initial_marking(high_in, 1);

  const petri::TransitionId low = ptpn.add_transition("low", petri::TimeInterval(4, 6), 1, 0, true);
  const petri::TransitionId high =
      ptpn.add_transition("high", petri::TimeInterval(2, 3), 9, 0, false);
  ptpn.set_pre_arc(low_in, low, 1);
  ptpn.set_post_arc(low, low_done, 1);
  ptpn.set_pre_arc(high_in, high, 1);
  ptpn.set_post_arc(high, high_done, 1);
  return ptpn;
}

TEST(DbmExtrapolationTest, RelaxesBoundsAboveKAndClampsBelowMinusK) {
  DBM dbm(3);
  // x1 in [7, 12], x2 in [1, 2], x1 - x2 in [5, 11].
  dbm.set_constraint(0, 1, -7);
  dbm.set_constraint(1, 0, 12);
  dbm.set_constraint(0, 2, -1);
  dbm.set_constraint(2, 0, 2);
  dbm.minimize();

  DBM original = dbm;
  dbm.extrapolate(5);

  // The extrapolated zone is a superset of the original zone.
  EXPECT_TRUE(original.included_in(dbm));
  // The upper bound of x1 (12 > k) is relaxed away.
  EXPECT_EQ(dbm.get_constraint(1, 0), state_class::INF_TIME);
  // The direct lower bound of x1 is clamped to -k, but re-canonicalization
  // re-tightens it through the retained difference x1 - x2 >= 5 and x2 >= 1,
  // giving x1 >= 6. Still looser than the original x1 >= 7.
  EXPECT_EQ(dbm.get_constraint(0, 1), -6);
  // Bounds already within [-k, k] survive.
  EXPECT_EQ(dbm.get_constraint(2, 0), 2);
  EXPECT_EQ(dbm.get_constraint(0, 2), -1);
}

TEST(DbmExtrapolationTest, FrozenClockRowsAndColumnsAreUntouched) {
  DBM dbm(3);
  dbm.set_constraint(0, 1, -7);
  dbm.set_constraint(1, 0, 12);
  dbm.set_constraint(0, 2, -8);
  dbm.set_constraint(2, 0, 9);
  dbm.minimize();
  dbm.freeze_clock(1);

  DBM original = dbm;
  dbm.extrapolate(5);

  // The extrapolated zone is a superset of the original zone.
  EXPECT_TRUE(original.included_in(dbm));
  // Clock 1 is frozen: its bounds survive even though they exceed k.
  EXPECT_EQ(dbm.get_constraint(1, 0), 12);
  EXPECT_EQ(dbm.get_constraint(0, 1), -7);
  // Clock 2 is not frozen: its direct bounds get extrapolated, then the
  // closure re-tightens the upper bound through the retained (frozen)
  // difference constraints: x2 <= (x2 - x1) + x1 <= 2 + 12 = 14. Still looser
  // than the original x2 <= 9.
  EXPECT_EQ(dbm.get_constraint(2, 0), 14);
  EXPECT_EQ(dbm.get_constraint(0, 2), -5);
}

TEST(ExtrapolationSemanticsTest, UnboundedIntervalNetBecomesFinite) {
  petri::PTPN without_net = make_unbounded_clock_net();
  StateClassReachabilityGraph without(without_net);
  without.build(200);
  // Without extrapolation the ticker keeps minting fresh classes forever.
  EXPECT_TRUE(without.get_statistics().truncated);

  petri::PTPN with_net = make_unbounded_clock_net();
  StateClassReachabilityGraph with(with_net);
  with.set_extrapolation(true);
  EXPECT_EQ(with.extrapolation_bound(), 5);
  with.build(200);
  EXPECT_FALSE(with.get_statistics().truncated);
  EXPECT_LT(with.get_statistics().total_states, 50u);

  // Extrapolation must preserve the reachable marking set exactly. The
  // truncated run has already visited every reachable marking (BFS, tiny
  // marking space), so the two sets must coincide.
  EXPECT_EQ(reachable_markings(without.get_graph()), reachable_markings(with.get_graph()));
}

TEST(ExtrapolationSemanticsTest, PreemptionNetKeepsExactStateGraph) {
  petri::PTPN plain_net = make_preemption_net();
  StateClassReachabilityGraph plain(plain_net);
  plain.build(10000);
  ASSERT_FALSE(plain.get_statistics().truncated);

  petri::PTPN extra_net = make_preemption_net();
  StateClassReachabilityGraph extra(extra_net);
  extra.set_extrapolation(true);
  extra.build(10000);
  ASSERT_FALSE(extra.get_statistics().truncated);

  // The net is already bounded, so extrapolation may only merge classes,
  // never invent or lose behavior: identical marking sets, no more states.
  EXPECT_EQ(reachable_markings(plain.get_graph()), reachable_markings(extra.get_graph()));
  EXPECT_LE(extra.get_statistics().total_states, plain.get_statistics().total_states);
}

TEST(ExtrapolationSemanticsTest, SBenchStyleFiniteNetIsUnchanged) {
  // For a net whose clocks never exceed the largest constant, extrapolation
  // must be a strict no-op: same states, same transitions.
  petri::PTPN plain_net = make_preemption_net();
  StateClassReachabilityGraph plain(plain_net);
  plain.build(10000);

  petri::PTPN extra_net = make_preemption_net();
  StateClassReachabilityGraph extra(extra_net);
  extra.set_extrapolation(true);
  extra.build(10000);

  EXPECT_EQ(extra.get_statistics().total_states, plain.get_statistics().total_states);
  EXPECT_EQ(extra.get_statistics().total_transitions, plain.get_statistics().total_transitions);
}

}  // namespace
