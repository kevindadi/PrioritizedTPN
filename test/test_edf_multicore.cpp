#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/dbm/dbm.h"
#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling_policy.h"
#include "analysis/reachability/state_class.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"

namespace {

using state_class::BranchConstraint;
using state_class::ClockKind;
using state_class::EdfFiringRule;
using state_class::MetricsReport;
using state_class::PolicyBranch;
using state_class::PolicyContext;
using state_class::StateClass;
using state_class::StateClassReachabilityGraph;
using state_class::TaskChain;
using state_class::TaskMetrics;

tdg::TDG parse_tdg(const std::string& json) {
  tdg::TDG tdg;
  tdg.parse_json_string(json);
  return tdg;
}

petri::PTPN lower(const tdg::TDG& tdg) {
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);
  return ptpn;
}

size_t find_transition(const petri::PTPN& net, const std::string& name) {
  for (size_t t = 0; t < net.num_transitions(); ++t) {
    if (net.get_transition(petri::TransitionId{t}).name == name) {
      return t;
    }
  }
  return SIZE_MAX;
}

// Position in the task's `node_pn_map` chain (0 = entry place).
size_t chain_transition(const petri::PTPN& net, const std::string& task, size_t position) {
  const auto it = net.node_pn_map().find(task);
  EXPECT_NE(it, net.node_pn_map().end()) << task;
  if (it == net.node_pn_map().end() || position >= it->second.size()) {
    return SIZE_MAX;
  }
  EXPECT_TRUE(it->second[position].is_transition()) << task << "[" << position << "]";
  return it->second[position].is_transition() ? it->second[position].as_transition().index()
                                              : SIZE_MAX;
}

MetricsReport analyze(const petri::PTPN& net, size_t max_states = 50000) {
  StateClassReachabilityGraph graph(net);
  graph.build(max_states);
  EXPECT_FALSE(graph.get_statistics().truncated);
  state_class::MetricsAnalyzer analyzer(graph.get_graph(), net, graph.get_initial_vertex(),
                                        /*exact=*/true);
  return analyzer.analyze();
}

const TaskMetrics& task_metrics(const MetricsReport& report, const std::string& name) {
  const auto it = std::find_if(report.tasks.begin(), report.tasks.end(),
                               [&](const TaskMetrics& task) { return task.name == name; });
  EXPECT_NE(it, report.tasks.end()) << "missing task metrics for " << name;
  static const TaskMetrics kEmpty;
  return it == report.tasks.end() ? kEmpty : *it;
}

size_t task_ordinal(const std::vector<TaskChain>& tasks, const std::string& name) {
  for (size_t i = 0; i < tasks.size(); ++i) {
    if (tasks[i].name == name) {
      return i;
    }
  }
  return SIZE_MAX;
}

void expect_rejected(const std::string& json, const std::string& message_substring) {
  try {
    (void)lower(parse_tdg(json));
    FAIL() << "expected lowering to reject the model";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find(message_substring), std::string::npos)
        << "unexpected message: " << error.what();
  }
}

// Two statically partitioned cores with two one-shot jobs each. The relative
// deadline orders disagree across cores (B is earliest on core 0, D on core 1),
// so a global EDF ordering would serialize them. Per-core EDF dispatches one
// job on each core concurrently:
//   t=0: B and D run (1 unit each)
//   t=1: A and C run (2 units each)
// Response times: A = 3, B = 1, C = 3, D = 1.
const char* kTwoCoresJson = R"({
  "graph": {"name": "EdfTwoCores"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "edf",
    "start": [
      {"task": "A", "tokens": 1},
      {"task": "B", "tokens": 1},
      {"task": "C", "tokens": 1},
      {"task": "D", "tokens": 1}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 10,
     "time": [[2, 2]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []},
    {"id": "C", "type": "task", "priority": 0, "core": 1, "deadline": 10,
     "time": [[2, 2]], "locks": []},
    {"id": "D", "type": "task", "priority": 0, "core": 1, "deadline": 2,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// Equal deadlines on both cores: every per-core tie is feasible, so the
// Cartesian product is 2 choices on core 0 x 2 choices on core 1 = 4 branches.
const char* kFourWayTieJson = R"({
  "graph": {"name": "EdfFourWayTie"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "edf",
    "start": [
      {"task": "A", "tokens": 1},
      {"task": "B", "tokens": 1},
      {"task": "C", "tokens": 1},
      {"task": "D", "tokens": 1}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []},
    {"id": "C", "type": "task", "priority": 0, "core": 1, "deadline": 5,
     "time": [[1, 1]], "locks": []},
    {"id": "D", "type": "task", "priority": 0, "core": 1, "deadline": 5,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// Core 0 is busy with two tied one-shot jobs; core 1 has no in-flight job but
// its periodic release `C_fire` is a structurally enabled control transition.
const char* kIdleCoreJson = R"({
  "graph": {"name": "EdfIdleCore"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "edf",
    "start": [
      {"task": "A", "tokens": 1},
      {"task": "B", "tokens": 1}
    ],
    "periodic": [
      {"task": "C", "period": 10}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []},
    {"id": "C", "type": "task", "priority": 0, "core": 1, "deadline": 10,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// Two cores with two started one-shot jobs each. At r = 0 only B can be
// earliest on core 0 and only C on core 1. Relaxing r_A to [0,4] makes both A
// and B feasible on core 0 while core 1 stays fixed to C: the core-0 order is
// independent of the core-1 order.
const char* kIndependentOrdersJson = R"({
  "graph": {"name": "EdfIndependentOrders"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "edf",
    "start": [
      {"task": "A", "tokens": 1},
      {"task": "B", "tokens": 1},
      {"task": "C", "tokens": 1},
      {"task": "D", "tokens": 1}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 10,
     "time": [[1, 1]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 8,
     "time": [[1, 1]], "locks": []},
    {"id": "C", "type": "task", "priority": 0, "core": 1, "deadline": 5,
     "time": [[1, 1]], "locks": []},
    {"id": "D", "type": "task", "priority": 0, "core": 1, "deadline": 7,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// ---------------------------------------------------------------------------
// 1. Per-core EDF selection, no cross-core ordering, known WCRT values
// ---------------------------------------------------------------------------

TEST(EdfMultiCoreTest, PerCoreSelectionProducesKnownResponseTimes) {
  const petri::PTPN net = lower(parse_tdg(kTwoCoresJson));
  ASSERT_EQ(net.schedule_policy(), SchedulePolicy::EDF);

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  ASSERT_EQ(tasks.size(), 4u);
  EXPECT_EQ(tasks[task_ordinal(tasks, "A")].core, 0);
  EXPECT_EQ(tasks[task_ordinal(tasks, "B")].core, 0);
  EXPECT_EQ(tasks[task_ordinal(tasks, "C")].core, 1);
  EXPECT_EQ(tasks[task_ordinal(tasks, "D")].core, 1);

  const MetricsReport report = analyze(net);
  const TaskMetrics& a = task_metrics(report, "A");
  const TaskMetrics& b = task_metrics(report, "B");
  const TaskMetrics& c = task_metrics(report, "C");
  const TaskMetrics& d = task_metrics(report, "D");

  // Both cores make progress in parallel: a single global EDF order would
  // serialize the two cores and inflate these values.
  EXPECT_EQ(a.wcrt.value, 3);
  EXPECT_EQ(b.wcrt.value, 1);
  EXPECT_EQ(c.wcrt.value, 3);
  EXPECT_EQ(d.wcrt.value, 1);
  EXPECT_FALSE(a.wcrt.infinite);
  EXPECT_FALSE(b.wcrt.infinite);
  EXPECT_FALSE(c.wcrt.infinite);
  EXPECT_FALSE(d.wcrt.infinite);
  EXPECT_TRUE(report.schedulable);
}

// ---------------------------------------------------------------------------
// 2. Per-core ties: the Cartesian product is enumerated
// ---------------------------------------------------------------------------

TEST(EdfMultiCoreTest, PerCoreTiesEnumerateCartesianProduct) {
  const petri::PTPN net = lower(parse_tdg(kFourWayTieJson));
  StateClassReachabilityGraph graph(net);

  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 4u);

  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t b_get_core = chain_transition(net, "B", 1);
  const size_t c_get_core = chain_transition(net, "C", 1);
  const size_t d_get_core = chain_transition(net, "D", 1);
  ASSERT_NE(a_get_core, SIZE_MAX);
  ASSERT_NE(b_get_core, SIZE_MAX);
  ASSERT_NE(c_get_core, SIZE_MAX);
  ASSERT_NE(d_get_core, SIZE_MAX);

  int core0_a = 0;
  int core0_b = 0;
  int core1_c = 0;
  int core1_d = 0;
  for (const StateClass& state : initials) {
    const bool picks_a = state_class::contains(state.priority_enabled, a_get_core);
    const bool picks_b = state_class::contains(state.priority_enabled, b_get_core);
    const bool picks_c = state_class::contains(state.priority_enabled, c_get_core);
    const bool picks_d = state_class::contains(state.priority_enabled, d_get_core);
    // Exactly one choice per core, and the choices are independent.
    EXPECT_NE(picks_a, picks_b);
    EXPECT_NE(picks_c, picks_d);
    core0_a += picks_a ? 1 : 0;
    core0_b += picks_b ? 1 : 0;
    core1_c += picks_c ? 1 : 0;
    core1_d += picks_d ? 1 : 0;
  }
  // The Cartesian product visits every combination twice.
  EXPECT_EQ(core0_a, 2);
  EXPECT_EQ(core0_b, 2);
  EXPECT_EQ(core1_c, 2);
  EXPECT_EQ(core1_d, 2);

  // The rule itself exposes exactly four branches, not one global tie-break.
  EdfFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(initials.front().struct_enabled, net, PolicyContext{net, &initials.front()});
  ASSERT_EQ(branches.size(), 4u);
  for (const PolicyBranch& branch : branches) {
    EXPECT_EQ(branch.active.size(), 2u);       // one get_core per core, no control
    EXPECT_EQ(branch.constraints.size(), 2u);  // one same-core pair per core
  }
}

// ---------------------------------------------------------------------------
// 3. One idle core: control transitions combine with the busy core choice
// ---------------------------------------------------------------------------

TEST(EdfMultiCoreTest, IdleCoreControlTransitionsCombineWithBusyCoreChoice) {
  const petri::PTPN net = lower(parse_tdg(kIdleCoreJson));
  StateClassReachabilityGraph graph(net);

  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 2u);  // two core-0 tie choices, core 1 has none

  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t b_get_core = chain_transition(net, "B", 1);
  const size_t c_get_core = chain_transition(net, "C", 1);
  const size_t c_fire = find_transition(net, "C_fire");
  ASSERT_NE(a_get_core, SIZE_MAX);
  ASSERT_NE(b_get_core, SIZE_MAX);
  ASSERT_NE(c_get_core, SIZE_MAX);
  ASSERT_NE(c_fire, SIZE_MAX);
  ASSERT_LT(net.get_transition(petri::TransitionId{c_fire}).core, 0);

  int a_branches = 0;
  int b_branches = 0;
  for (const StateClass& state : initials) {
    // The control release is active in every branch and is combined with the
    // busy core's choice.
    EXPECT_TRUE(state_class::contains(state.priority_enabled, c_fire));
    const bool picks_a = state_class::contains(state.priority_enabled, a_get_core);
    const bool picks_b = state_class::contains(state.priority_enabled, b_get_core);
    EXPECT_NE(picks_a, picks_b);
    // The idle core contributes no chain transition.
    EXPECT_FALSE(state_class::contains(state.priority_enabled, c_get_core));
    a_branches += picks_a ? 1 : 0;
    b_branches += picks_b ? 1 : 0;
  }
  EXPECT_EQ(a_branches, 1);
  EXPECT_EQ(b_branches, 1);
}

// ---------------------------------------------------------------------------
// 4. Deadline orders on different cores are independent
// ---------------------------------------------------------------------------

TEST(EdfMultiCoreTest, DeadlineOrdersAreIndependentAcrossCores) {
  const petri::PTPN net = lower(parse_tdg(kIndependentOrdersJson));
  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 1u);  // at r = 0 only B (core 0) and C (core 1)

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  const size_t ordinal_c = task_ordinal(tasks, "C");
  const size_t ordinal_d = task_ordinal(tasks, "D");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);
  ASSERT_NE(ordinal_c, SIZE_MAX);
  ASSERT_NE(ordinal_d, SIZE_MAX);

  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t b_get_core = chain_transition(net, "B", 1);
  const size_t c_get_core = chain_transition(net, "C", 1);
  const size_t d_get_core = chain_transition(net, "D", 1);
  ASSERT_NE(a_get_core, SIZE_MAX);
  ASSERT_NE(b_get_core, SIZE_MAX);
  ASSERT_NE(c_get_core, SIZE_MAX);
  ASSERT_NE(d_get_core, SIZE_MAX);

  // Release A late enough that both core-0 orders are feasible. Core 1 keeps
  // its initial ordering (C earliest): its clocks stay pinned to zero.
  StateClass relaxed = initials.front();
  const int r_a = relaxed.release_index(ordinal_a);
  ASSERT_GT(r_a, 0);
  const size_t r_a_index = static_cast<size_t>(r_a);
  relaxed.zone.set_constraint(r_a_index, 0, 4);
  for (size_t j = 1; j < relaxed.zone.size(); ++j) {
    if (j != r_a_index) {
      relaxed.zone.set_constraint(r_a_index, j, 4);
    }
  }
  relaxed.zone.minimize();
  ASSERT_TRUE(relaxed.zone.is_consistent());

  EdfFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(relaxed.struct_enabled, net, PolicyContext{net, &relaxed});
  ASSERT_EQ(branches.size(), 2u);  // {A, C} and {B, C}

  const PolicyBranch* a_branch = nullptr;
  const PolicyBranch* b_branch = nullptr;
  for (const PolicyBranch& branch : branches) {
    const bool picks_a = state_class::contains(branch.active, a_get_core);
    const bool picks_b = state_class::contains(branch.active, b_get_core);
    EXPECT_NE(picks_a, picks_b);
    // Core 1 always picks C in both branches: the core-0 order did not leak.
    EXPECT_TRUE(state_class::contains(branch.active, c_get_core));
    EXPECT_FALSE(state_class::contains(branch.active, d_get_core));
    if (picks_a) {
      a_branch = &branch;
    } else {
      b_branch = &branch;
    }
  }
  ASSERT_NE(a_branch, nullptr);
  ASSERT_NE(b_branch, nullptr);

  ASSERT_EQ(a_branch->constraints.size(), 2u);
  ASSERT_EQ(b_branch->constraints.size(), 2u);

  // Core 0: A earliest iff r_B - r_A <= D_B - D_A = -2; B earliest iff
  // r_A - r_B <= D_A - D_B = 2.
  const auto& a_core0 = a_branch->constraints.front();
  EXPECT_EQ(a_core0.domain, ClockKind::Release);
  EXPECT_EQ(a_core0.left_task, ordinal_a);
  EXPECT_EQ(a_core0.right_task, ordinal_b);
  EXPECT_EQ(a_core0.bound, -2);
  const auto& b_core0 = b_branch->constraints.front();
  EXPECT_EQ(b_core0.domain, ClockKind::Release);
  EXPECT_EQ(b_core0.left_task, ordinal_b);
  EXPECT_EQ(b_core0.right_task, ordinal_a);
  EXPECT_EQ(b_core0.bound, 2);

  // Core 1: C earliest iff r_D - r_C <= D_D - D_C = 2, in both branches.
  for (const PolicyBranch* branch : {a_branch, b_branch}) {
    const BranchConstraint& core1 = branch->constraints.back();
    EXPECT_EQ(core1.domain, ClockKind::Release);
    EXPECT_EQ(core1.left_task, ordinal_c);
    EXPECT_EQ(core1.right_task, ordinal_d);
    EXPECT_EQ(core1.bound, 2);
  }
}

// ---------------------------------------------------------------------------
// 5. Direct branch check: no cross-core clock comparisons
// ---------------------------------------------------------------------------

TEST(EdfMultiCoreTest, BranchConstraintsNeverReferenceCrossCorePairs) {
  const petri::PTPN net = lower(parse_tdg(kFourWayTieJson));
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);

  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_FALSE(initials.empty());

  EdfFiringRule rule(net);
  for (const StateClass& state : initials) {
    const std::vector<PolicyBranch> branches =
        rule.branches(state.struct_enabled, net, PolicyContext{net, &state});
    ASSERT_FALSE(branches.empty());
    for (const PolicyBranch& branch : branches) {
      ASSERT_EQ(branch.constraints.size(), 2u);  // one per core
      for (const BranchConstraint& constraint : branch.constraints) {
        ASSERT_LT(constraint.left_task, tasks.size());
        ASSERT_LT(constraint.right_task, tasks.size());
        EXPECT_EQ(tasks[constraint.left_task].core, tasks[constraint.right_task].core)
            << "constraint compares tasks on different cores";
        EXPECT_EQ(constraint.domain, ClockKind::Release);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 6. Multi-core guards
// ---------------------------------------------------------------------------

TEST(EdfMultiCoreTest, RejectsUnsupportedMultiCoreModels) {
  // num_cpus below one.
  expect_rejected(R"({
    "graph": {"name": "EdfNoCpus"},
    "configuration": {"num_cpus": 0, "cores_per_cpu": 1, "policy": "edf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "num_cpus >= 1");

  // Task core outside the declared core range.
  expect_rejected(R"({
    "graph": {"name": "EdfCoreOutOfRange"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "edf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 2,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "outside [0, 2)");

  // More than one core per CPU (per-core parallelism is not modeled).
  expect_rejected(R"({
    "graph": {"name": "EdfCoresPerCpu"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 2, "policy": "edf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "cores_per_cpu == 1");

  // Fork/join.
  expect_rejected(R"({
    "graph": {"name": "EdfFork"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "edf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0,
       "deadline": 5, "time": [[1, 1]], "locks": []},
      {"id": "F", "type": "fork", "core": 0, "time": [[1, 1]]}
    ],
    "edges": []
  })",
                  "fork/join");

  // Locks.
  expect_rejected(R"({
    "graph": {"name": "EdfLocksMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "edf",
                      "shared_locks": ["m"],
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": ["m"]}],
    "edges": []
  })",
                  "does not include locks");

  // Dependency-driven release between tasks on different cores.
  expect_rejected(R"({
    "graph": {"name": "EdfDependencyMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "edf",
                      "periodic": [{"task": "A", "period": 10},
                                   {"task": "B", "period": 10}]},
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0,
       "time": [[1, 1]], "locks": []},
      {"id": "B", "type": "task", "priority": 0, "core": 1,
       "time": [[1, 1]], "locks": []}
    ],
    "edges": [{"source": "A", "target": "B", "label": "10"}]
  })",
                  "dependency-driven releases");

  // Capacity above one (queued jobs are outside the supported class).
  expect_rejected(R"({
    "graph": {"name": "EdfCapacityTwoMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "edf",
                      "task_place_capacity": 2,
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "task_place_capacity == 1");

  // More than one initial job for a task.
  expect_rejected(R"({
    "graph": {"name": "EdfTwoStartsMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "edf",
                      "start": [{"task": "A", "tokens": 2}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "at most one initial job");

  // A periodic release that overlaps a mid-chain job is rejected at the
  // successor state instead of being queued into the single job slot.
  const petri::PTPN overlap_net = lower(parse_tdg(R"({
    "graph": {"name": "EdfOverlapMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "edf",
                      "periodic": [{"task": "A", "period": 2}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 1,
               "deadline": 4, "time": [[3, 3]], "locks": []}],
    "edges": []
  })"));
  StateClassReachabilityGraph overlap_graph(overlap_net);
  try {
    overlap_graph.build(10000);
    FAIL() << "expected the overlapping release to be rejected";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("one job per task"), std::string::npos)
        << "unexpected message: " << error.what();
  }
}

// A net whose TaskInfo core disagrees with its chain transitions is rejected by
// the rule itself, even when the lowering validator is bypassed.
TEST(EdfMultiCoreTest, RejectsChainTransitionOnWrongCore) {
  petri::PTPN net;
  const petri::PlaceId entry = net.add_place("A_entry", 1, true);
  const petri::TransitionId get_core =
      net.add_transition("A_get_core", petri::TimeInterval(0, 0), 0, 0, false);
  const petri::PlaceId ready = net.add_place("A_ready", 1, true);
  const petri::TransitionId exec =
      net.add_transition("A_exec", petri::TimeInterval(1, 1), 0, 1, true);
  const petri::PlaceId exit = net.add_place("A_exit", 1, true);
  net.set_pre_arc(entry, get_core);
  net.set_post_arc(get_core, ready);
  net.set_pre_arc(ready, exec);
  net.set_post_arc(exec, exit);
  net.set_initial_marking(entry, 1);
  net.set_task_chain(
      "A", {petri::NodeRef::of(entry), petri::NodeRef::of(get_core), petri::NodeRef::of(ready),
            petri::NodeRef::of(exec), petri::NodeRef::of(exit)});
  petri::TaskInfo info;
  info.core = 0;
  info.deadline = 5;
  info.wcet = 1;
  net.set_task_info("A", info);
  net.set_schedule_policy(SchedulePolicy::EDF);

  try {
    EdfFiringRule rule(net);
    FAIL() << "expected EDF to reject a chain transition on the wrong core";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("chain transition"), std::string::npos)
        << "unexpected message: " << error.what();
  }
}

}  // namespace
