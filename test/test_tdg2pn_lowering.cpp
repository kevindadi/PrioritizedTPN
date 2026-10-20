#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>

#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "parse/json.h"

namespace {

petri::PTPN lower(const std::string& json) {
  tdg::TDG tdg;
  parse::load_tdg_from_string(json, tdg);
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

TEST(Tdg2pnLoweringTest, ExplicitDeadlineIsPropagatedToTaskInfo) {
  const std::string explicit_deadline = R"({
    "graph": {"name": "Deadline"},
    "configuration": {
      "num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed",
      "periodic": [{"task": "A", "period": 10}]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0, "deadline": 6,
       "time": [[2, 2]], "locks": []}
    ],
    "edges": []
  })";
  const petri::PTPN explicit_net = lower(explicit_deadline);
  const auto& explicit_info = explicit_net.task_info().at("A");
  EXPECT_EQ(explicit_info.period, 10);
  EXPECT_EQ(explicit_info.deadline, 6);

  const std::string implicit_deadline = R"({
    "graph": {"name": "Deadline"},
    "configuration": {
      "num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed",
      "periodic": [{"task": "A", "period": 10}]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0,
       "time": [[2, 2]], "locks": []}
    ],
    "edges": []
  })";
  const petri::PTPN implicit_net = lower(implicit_deadline);
  EXPECT_EQ(implicit_net.task_info().at("A").deadline, 10);
}

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

TEST(Tdg2pnLoweringTest, SpinLockCriticalSectionIsNotSuspendable) {
  // One spin lock -> three execution segments. The lock is acquired after
  // segment 1 and returned by segment 2, so segment 2 is the critical section
  // and only it must keep the CPU.
  const std::string spin_lock = R"({
    "graph": {"name": "SpinLock"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed"},
    "nodes": [
      {"id": "W", "type": "task", "priority": 1, "core": 0,
       "time": [[1, 1], [2, 2], [1, 1]], "locks": ["spin1"]}
    ],
    "edges": []
  })";
  const petri::PTPN spin_net = lower(spin_lock);
  EXPECT_TRUE(spin_net.get_transition(petri::TransitionId{find_transition(spin_net, "W_exec_1")})
                  .suspendable);
  EXPECT_FALSE(spin_net.get_transition(petri::TransitionId{find_transition(spin_net, "W_exec_2")})
                   .suspendable);
  EXPECT_TRUE(spin_net.get_transition(petri::TransitionId{find_transition(spin_net, "W_exec_3")})
                  .suspendable);

  // The same chain with a mutex keeps every segment suspendable.
  std::string mutex_lock = spin_lock;
  const std::size_t position = mutex_lock.find("spin1");
  ASSERT_NE(std::string::npos, position);
  mutex_lock.replace(position, 5, "mutex1");
  const petri::PTPN mutex_net = lower(mutex_lock);
  for (const char* name : {"W_exec_1", "W_exec_2", "W_exec_3"}) {
    EXPECT_TRUE(mutex_net.get_transition(petri::TransitionId{find_transition(mutex_net, name)})
                    .suspendable)
        << name;
  }
}

TEST(Tdg2pnLoweringTest, NestedLocksKeepEveryCoveredSegmentNonSuspendable) {
  // Two locks are acquired in order and released in reverse (LIFO): the outer
  // spin lock covers segments 2, 3 and 4 of the five-segment chain, the inner
  // mutex only segment 3. Every segment covered by the spin lock must keep the
  // CPU.
  const std::string nested = R"({
    "graph": {"name": "NestedLocks"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed"},
    "nodes": [
      {"id": "W", "type": "task", "priority": 1, "core": 0,
       "time": [[1, 1], [2, 2], [3, 3], [2, 2], [1, 1]], "locks": ["spin1", "mutex1"]}
    ],
    "edges": []
  })";
  const petri::PTPN net = lower(nested);
  const bool expected[] = {true, false, false, false, true};
  for (std::size_t index = 0; index < 5; ++index) {
    const std::string name = "W_exec_" + std::to_string(index + 1);
    EXPECT_EQ(expected[index],
              net.get_transition(petri::TransitionId{find_transition(net, name)}).suspendable)
        << name;
  }
}
