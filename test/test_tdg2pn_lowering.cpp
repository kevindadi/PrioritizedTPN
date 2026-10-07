#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>

#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"

namespace {

petri::PTPN lower(const std::string& json) {
  tdg::TDG tdg;
  tdg.parse_json_string(json);
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);
  return ptpn;
}

size_t find_place(const petri::PTPN& net, const std::string& name) {
  for (size_t p = 0; p < net.num_places(); ++p) {
    if (net.get_place(petri::PlaceId{p}).name == name) {
      return p;
    }
  }
  return SIZE_MAX;
}

size_t find_transition(const petri::PTPN& net, const std::string& name) {
  for (size_t t = 0; t < net.num_transitions(); ++t) {
    if (net.get_transition(petri::TransitionId{t}).name == name) {
      return t;
    }
  }
  return SIZE_MAX;
}

const char* kBaseGraph = R"({
  "graph": {"name": "SelfLoop"},
  "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed"},
  "nodes": [
    {"id": "A", "type": "task", "priority": 1, "core": 0, "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

const char* kSelfLoopGraph = R"({
  "graph": {"name": "SelfLoop"},
  "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed"},
  "nodes": [
    {"id": "A", "type": "task", "priority": 1, "core": 0, "time": [[2, 2]], "locks": []}
  ],
  "edges": [{"source": "A", "target": "A", "label": "10"}]
})";

}  // namespace

TEST(Tdg2pnLoweringTest, SelfLoopEdgeIsIgnored) {
  const petri::PTPN plain = lower(kBaseGraph);
  const petri::PTPN looped = lower(kSelfLoopGraph);

  // The self-loop is an allowed annotation but lowers to nothing: same net,
  // and no monitor sub-net places/transitions appear.
  EXPECT_EQ(looped.num_places(), plain.num_places());
  EXPECT_EQ(looped.num_transitions(), plain.num_transitions());
  EXPECT_EQ(find_place(looped, "Adeadline"), SIZE_MAX);
  EXPECT_EQ(find_place(looped, "Atimeout"), SIZE_MAX);
  EXPECT_EQ(find_transition(looped, "Acomplete"), SIZE_MAX);
  EXPECT_EQ(find_transition(looped, "Aout"), SIZE_MAX);
}

TEST(Tdg2pnLoweringTest, PeriodicReleaseAppliesDespiteSelfLoopEdge) {
  const std::string json = R"({
    "graph": {"name": "SelfLoopPeriodic"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "fixed",
      "periodic": [{"task": "A", "period": 10}]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0, "time": [[2, 2]], "locks": []}
    ],
    "edges": [{"source": "A", "target": "A", "label": "10"}]
  })";

  const petri::PTPN net = lower(json);
  EXPECT_NE(find_place(net, "A_period"), SIZE_MAX);
  EXPECT_NE(find_transition(net, "A_fire"), SIZE_MAX);
}
