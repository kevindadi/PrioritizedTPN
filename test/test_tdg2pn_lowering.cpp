#include <cstddef>
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

}  // namespace

TEST(Tdg2pnLoweringTest, SelfLoopMonitorCompletionCancelsDeadline) {
  const std::string json = R"({
    "graph": {"name": "Monitor"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed"},
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0, "time": [[2, 2]], "locks": []}
    ],
    "edges": [{"source": "A", "target": "A", "label": "10"}]
  })";

  const petri::PTPN net = lower(json);

  const size_t deadline = find_place(net, "Adeadline");
  const size_t end_place = find_place(net, "Aend");
  const size_t complete = find_transition(net, "Acomplete");
  const size_t timeout_transition = find_transition(net, "Aout");
  ASSERT_NE(deadline, SIZE_MAX);
  ASSERT_NE(end_place, SIZE_MAX);
  ASSERT_NE(complete, SIZE_MAX);
  ASSERT_NE(timeout_transition, SIZE_MAX);

  const auto& pre = net.get_pre_matrix();
  // Completion consumes the end marker and cancels the pending deadline.
  EXPECT_EQ(pre[end_place][complete], 1);
  EXPECT_EQ(pre[deadline][complete], 1);
  // The timeout transition competes for the same deadline token.
  EXPECT_EQ(pre[deadline][timeout_transition], 1);
}
