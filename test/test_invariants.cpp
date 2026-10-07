#include <algorithm>
#include <boost/graph/adjacency_list.hpp>
#include <cstddef>
#include <gtest/gtest.h>
#include <string>

#include "analysis/dbm.h"
#include "analysis/ptpn_analysis.h"
#include "analysis/state_class.h"
#include "export/scg_dot.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "tdg_test_helpers.h"

namespace {

using state_class::StateClass;
using state_class::StateClassReachabilityGraph;
using state_class::TransitionSet;

const char* kExamples[] = {
    "example/motivating-examples/hw.json", "example/s-bench/a.json",
    "example/p-bench/initial.json",        "example/p-bench/same-core.json",
    "example/p-bench/no-period.json",
};

petri::PTPN lower_example(const std::string& path) {
  tdg::TDG tdg = ptpn_test::load_tdg(path);
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);
  return ptpn;
}

bool is_sorted_subset(const TransitionSet& subset, const TransitionSet& superset) {
  return std::includes(superset.begin(), superset.end(), subset.begin(), subset.end());
}

bool are_disjoint(const TransitionSet& left, const TransitionSet& right) {
  for (size_t value : left) {
    if (state_class::contains(right, value)) {
      return false;
    }
  }
  return true;
}

TEST(InvariantsTest, NetStructureIsConsistent) {
  for (const char* path : kExamples) {
    SCOPED_TRACE(path);
    const petri::PTPN net = lower_example(path);

    ASSERT_TRUE(net.verify_structure());
    EXPECT_EQ(net.get_pre_matrix().size(), net.num_places());
    for (const auto& row : net.get_pre_matrix()) {
      EXPECT_EQ(row.size(), net.num_transitions());
    }
    EXPECT_EQ(net.get_post_matrix().size(), net.num_transitions());
    for (const auto& row : net.get_post_matrix()) {
      EXPECT_EQ(row.size(), net.num_places());
    }
    EXPECT_EQ(net.get_marking().size(), net.num_places());

    // Sparse arcs must agree with the dense matrices, entry by entry.
    for (size_t t = 0; t < net.num_transitions(); ++t) {
      size_t pre_sparse = 0;
      for (const auto& [place, weight] : net.pre_arcs()[t]) {
        EXPECT_EQ(net.get_pre_matrix()[place][t], weight);
        ++pre_sparse;
      }
      size_t pre_dense = 0;
      for (size_t p = 0; p < net.num_places(); ++p) {
        pre_dense += net.get_pre_matrix()[p][t] > 0 ? 1 : 0;
      }
      EXPECT_EQ(pre_sparse, pre_dense);

      size_t post_sparse = 0;
      for (const auto& [place, weight] : net.post_arcs()[t]) {
        EXPECT_EQ(net.get_post_matrix()[t][place], weight);
        ++post_sparse;
      }
      size_t post_dense = 0;
      for (size_t p = 0; p < net.num_places(); ++p) {
        post_dense += net.get_post_matrix()[t][p] > 0 ? 1 : 0;
      }
      EXPECT_EQ(post_sparse, post_dense);
    }
  }
}

TEST(InvariantsTest, StateClassSetsLayoutAndZoneAreConsistent) {
  for (const char* path : kExamples) {
    SCOPED_TRACE(path);
    const petri::PTPN net = lower_example(path);
    StateClassReachabilityGraph graph(net);
    graph.build(5000);
    const auto& scg = graph.get_graph();

    size_t vertex_count = 0;
    for (auto [vi, vend] = boost::vertices(scg); vi != vend; ++vi) {
      ++vertex_count;
      const StateClass& state = boost::get(boost::vertex_name, scg, *vi);

      // E_struct(M) is exactly the set of transitions enabled by the marking.
      size_t enabled_count = 0;
      for (size_t t = 0; t < net.num_transitions(); ++t) {
        if (petri::PTPN::is_enabled(state.marking, net, petri::TransitionId{t})) {
          ++enabled_count;
          EXPECT_TRUE(state_class::contains(state.struct_enabled, t));
        }
      }
      EXPECT_EQ(enabled_count, state.struct_enabled.size());

      // Priority/suspension sets refine the structural set and are disjoint.
      EXPECT_TRUE(is_sorted_subset(state.priority_enabled, state.struct_enabled));
      EXPECT_TRUE(is_sorted_subset(state.suspended, state.struct_enabled));
      EXPECT_TRUE(are_disjoint(state.priority_enabled, state.suspended));

      // Clock layout mirrors the net and the zone dimension.
      EXPECT_EQ(state.exec_clock_of_transition.size(), net.num_transitions());
      EXPECT_EQ(state.susp_clock_of_transition.size(), net.num_transitions());
      EXPECT_EQ(state.clock_vars.size(), state.zone.size());
      ASSERT_FALSE(state.clock_vars.empty());
      EXPECT_EQ(state.clock_vars[0].kind, state_class::ClockKind::Zero);

      // Zones are non-empty, consistent, and already canonical.
      EXPECT_FALSE(state.zone.is_empty());
      EXPECT_TRUE(state.zone.is_consistent());
      state_class::DBM copy = state.zone;
      ASSERT_TRUE(copy.minimize_and_check());
      EXPECT_EQ(copy.raw_matrix(), state.zone.raw_matrix());
    }

    size_t edge_count = 0;
    for (auto [ei, eend] = boost::edges(scg); ei != eend; ++ei) {
      ++edge_count;
      const auto source = boost::source(*ei, scg);
      const StateClass& state = boost::get(boost::vertex_name, scg, source);
      const auto& edge = boost::get(boost::edge_name, scg, *ei);
      EXPECT_TRUE(
          state_class::contains(state.priority_enabled, static_cast<size_t>(edge.transition_id)));
    }

    EXPECT_EQ(graph.get_statistics().total_states, vertex_count);
    EXPECT_EQ(graph.get_statistics().total_transitions, edge_count);
  }
}

TEST(InvariantsTest, ReachabilityBuildIsDeterministic) {
  for (const char* path : kExamples) {
    SCOPED_TRACE(path);
    const petri::PTPN first_net = lower_example(path);
    StateClassReachabilityGraph first(first_net);
    first.build(5000);

    const petri::PTPN second_net = lower_example(path);
    StateClassReachabilityGraph second(second_net);
    second.build(5000);

    EXPECT_EQ(first.get_statistics().total_states, second.get_statistics().total_states);
    EXPECT_EQ(first.get_statistics().total_transitions, second.get_statistics().total_transitions);
    EXPECT_EQ(first.get_statistics().dedup_hits, second.get_statistics().dedup_hits);

    const scg_export::ScgFormatter first_formatter(first_net, first.get_graph());
    const scg_export::ScgFormatter second_formatter(second_net, second.get_graph());
    EXPECT_EQ(first_formatter.render_dot(), second_formatter.render_dot());
  }
}

}  // namespace
