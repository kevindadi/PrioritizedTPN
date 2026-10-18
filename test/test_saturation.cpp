#include <gtest/gtest.h>
#include <string>

#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "parse/json.h"

namespace {

// producer: (src) -> [t] -> (dst). dst capacity 1.
petri::PTPN make_producer_net(bool saturating_dst) {
  petri::PTPN ptpn;
  const petri::PlaceId src = ptpn.add_place("src", 2);
  const petri::PlaceId dst = ptpn.add_place("dst", 1, saturating_dst);
  const petri::TransitionId t = ptpn.add_transition("t", petri::TimeInterval(0, 0));
  ptpn.set_pre_arc(src, t, 1);
  ptpn.set_post_arc(t, dst, 1);
  ptpn.set_initial_marking(src, 2);
  ptpn.set_initial_marking(dst, 1);  // dst already full
  return ptpn;
}

TEST(SaturationTest, NonSaturatingFullPlaceKeepsProducerEnabledAndClampsTokens) {
  // Enabling is input-driven: a full successor place never disables the
  // producer. Firing proceeds and the overflow is clamped to capacity.
  const petri::PTPN ptpn = make_producer_net(/*saturating_dst=*/false);
  ASSERT_TRUE(petri::PTPN::is_enabled(ptpn.get_marking(), ptpn, petri::TransitionId{0}));

  petri::reset_overflow_recording();
  const petri::Marking after = petri::PTPN::fire(ptpn.get_marking(), ptpn, petri::TransitionId{0});
  EXPECT_EQ(after[0], 1);  // src consumed one token
  EXPECT_EQ(after[1], 1);  // dst clamped at capacity
  // The clamp on a non-saturating place is an invalid behavior and is recorded.
  EXPECT_EQ(petri::overflowed_places(), (std::vector<size_t>{1}));
}

TEST(SaturationTest, SaturatingFullPlaceKeepsProducerEnabledAndClampsTokens) {
  const petri::PTPN ptpn = make_producer_net(/*saturating_dst=*/true);
  ASSERT_TRUE(petri::PTPN::is_enabled(ptpn.get_marking(), ptpn, petri::TransitionId{0}));

  petri::reset_overflow_recording();
  const petri::Marking after = petri::PTPN::fire(ptpn.get_marking(), ptpn, petri::TransitionId{0});
  EXPECT_EQ(after[0], 1);  // src consumed one token
  EXPECT_EQ(after[1], 1);  // dst clamped at capacity, overflow absorbed
  // Saturating places clamp silently; no invalid-behavior record.
  EXPECT_TRUE(petri::overflowed_places().empty());
}

TEST(SaturationTest, SaturatingPlaceBelowCapacityAccumulatesNormally) {
  petri::PTPN ptpn;
  const petri::PlaceId src = ptpn.add_place("src", 2);
  const petri::PlaceId dst = ptpn.add_place("dst", 2, /*saturate=*/true);
  const petri::TransitionId t = ptpn.add_transition("t", petri::TimeInterval(0, 0));
  ptpn.set_pre_arc(src, t, 1);
  ptpn.set_post_arc(t, dst, 1);
  ptpn.set_initial_marking(src, 2);
  ptpn.set_initial_marking(dst, 1);

  petri::Marking m = petri::PTPN::fire(ptpn.get_marking(), ptpn, petri::TransitionId{0});
  EXPECT_EQ(m[dst.index()], 2);  // below capacity: normal accumulation
  m = petri::PTPN::fire(m, ptpn, petri::TransitionId{0});
  EXPECT_EQ(m[dst.index()], 2);  // at capacity: saturated
  EXPECT_EQ(m[src.index()], 0);
}

TEST(JsonTaskPlaceCapacityTest, ParsesConfiguredValueAndDefault) {
  const std::string with_value = R"({
    "graph": {"name": "CapTest"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "task_place_capacity": 2},
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0, "time": [[1, 2]], "locks": []}
    ],
    "edges": []
  })";

  parse::Parser parser;
  ASSERT_TRUE(parser.parse_string(with_value).success);
  EXPECT_EQ(parser.get_task_place_capacity(), 2);
  EXPECT_TRUE(parser.validate().success);

  const std::string without_value = R"({
    "graph": {"name": "CapDefault"},
    "configuration": {"num_cpus": 1},
    "nodes": [], "edges": []
  })";
  parse::Parser default_parser;
  ASSERT_TRUE(default_parser.parse_string(without_value).success);
  EXPECT_EQ(default_parser.get_task_place_capacity(), 1);
}

TEST(JsonTaskPlaceCapacityTest, RejectsNonPositiveCapacity) {
  const std::string json = R"({
    "graph": {"name": "CapBad"},
    "configuration": {"num_cpus": 1, "task_place_capacity": 0},
    "nodes": [], "edges": []
  })";

  parse::Parser parser;
  ASSERT_TRUE(parser.parse_string(json).success);
  const auto validation = parser.validate();
  EXPECT_FALSE(validation.success);
}

// Periodic task whose period (5) is far shorter than its execution time (20):
// under the blocking semantics the release transition would be disabled while
// the entry place still holds a token; under saturating semantics it stays
// enabled and the entry marking is clamped at task_place_capacity.
petri::PTPN lower_periodic_net(int task_place_capacity) {
  const std::string json = R"({
    "graph": {"name": "PeriodicSaturation"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "fixed",
      "task_place_capacity": )" +
                           std::to_string(task_place_capacity) + R"(,
      "start": [{"task": "A", "tokens": 1}],
      "end": ["A"],
      "periodic": [{"task": "A", "period": 5}]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0, "time": [[20, 20]], "locks": []}
    ],
    "edges": []
  })";

  tdg::TDG tdg;
  tdg.parse_json_string(json);
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);
  return ptpn;
}

TEST(PeriodicSaturationTest, ReleaseStaysEnabledWhenEntryIsFull) {
  const petri::PTPN ptpn = lower_periodic_net(/*task_place_capacity=*/1);

  size_t entry = SIZE_MAX;
  size_t fire = SIZE_MAX;
  for (size_t p = 0; p < ptpn.num_places(); ++p) {
    if (ptpn.get_place(petri::PlaceId{p}).name == "Aentry") {
      entry = p;
    }
  }
  for (size_t t = 0; t < ptpn.num_transitions(); ++t) {
    if (ptpn.get_transition(petri::TransitionId{t}).name == "A_fire") {
      fire = t;
    }
  }
  ASSERT_NE(entry, SIZE_MAX);
  ASSERT_NE(fire, SIZE_MAX);

  const auto& entry_place = ptpn.get_place(petri::PlaceId{entry});
  EXPECT_TRUE(entry_place.saturate);
  EXPECT_EQ(entry_place.capacity, 1);

  // Entry starts with a token (start binding); the release must not be blocked.
  ASSERT_EQ(ptpn.get_marking()[entry], 1);
  ASSERT_TRUE(petri::PTPN::is_enabled(ptpn.get_marking(), ptpn, petri::TransitionId{fire}));

  const petri::Marking after =
      petri::PTPN::fire(ptpn.get_marking(), ptpn, petri::TransitionId{fire});
  EXPECT_EQ(after[entry], 1);  // release merged, marking stays at capacity
  // The release transition remains enabled for the next period.
  EXPECT_TRUE(petri::PTPN::is_enabled(after, ptpn, petri::TransitionId{fire}));
}

TEST(PeriodicSaturationTest, CapacityTwoAllowsOnePendingRelease) {
  const petri::PTPN ptpn = lower_periodic_net(/*task_place_capacity=*/2);

  size_t entry = SIZE_MAX;
  size_t fire = SIZE_MAX;
  for (size_t p = 0; p < ptpn.num_places(); ++p) {
    if (ptpn.get_place(petri::PlaceId{p}).name == "Aentry") {
      entry = p;
    }
  }
  for (size_t t = 0; t < ptpn.num_transitions(); ++t) {
    if (ptpn.get_transition(petri::TransitionId{t}).name == "A_fire") {
      fire = t;
    }
  }
  ASSERT_NE(entry, SIZE_MAX);
  ASSERT_NE(fire, SIZE_MAX);
  EXPECT_EQ(ptpn.get_place(petri::PlaceId{entry}).capacity, 2);

  petri::Marking m = ptpn.get_marking();
  ASSERT_EQ(m[entry], 1);
  m = petri::PTPN::fire(m, ptpn, petri::TransitionId{fire});
  EXPECT_EQ(m[entry], 2);  // one pending release queued
  m = petri::PTPN::fire(m, ptpn, petri::TransitionId{fire});
  EXPECT_EQ(m[entry], 2);  // further releases merged at the bound
}

}  // namespace
