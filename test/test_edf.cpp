#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/dbm/dbm.h"
#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"
#include "analysis/reachability/scheduling_policy.h"
#include "analysis/reachability/state_class.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "parse/json.h"

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
  parse::load_tdg_from_string(json, tdg);
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

// Two independent periodic tasks on one core, no initial starts:
//   A: period/deadline 10, execution [2,2]
//   B: period/deadline 5,  execution [1,1]
// B is released at 5, 10, ...; A at 10, 20, ...
// At t=10 B (absolute deadline 15) runs before A (absolute deadline 20), so
// A's worst response is: release 10, wait 1 behind B, execute 2 -> 3.
const char* kTwoPeriodicJson = R"({
  "graph": {"name": "EdfTwoPeriodic"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "edf",
    "periodic": [
      {"task": "A", "period": 10},
      {"task": "B", "period": 5}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 10,
     "time": [[2, 2]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// EDF differs from RM/DM here because the periodic task has the shorter
// period/deadline but its *absolute* deadline at t=5 (10) is later than the
// running one-shot job's (8):
//   A: periodic period/deadline 5, execution [1,1]
//   B: one-shot start at 0, deadline 8, execution [6,6]
// RM/DM rank A above B: A preempts at 5, so B completes at 7 (response 7).
// EDF keeps B until 6, then runs A; A responds in 2 and B in 6.
std::string edf_vs_static_json(const std::string& policy) {
  return R"({
    "graph": {"name": "EdfVsStatic"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": ")" +
         policy + R"(",
      "start": [{"task": "B", "tokens": 1}],
      "periodic": [
        {"task": "A", "period": 5}
      ]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 5,
       "time": [[1, 1]], "locks": []},
      {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 8,
       "time": [[6, 6]], "locks": []}
    ],
    "edges": []
  })";
}

// Equal absolute deadlines at time 0: both tie resolutions must be enumerated.
const char* kEqualDeadlineJson = R"({
  "graph": {"name": "EdfEqualDeadline"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "edf",
    "start": [
      {"task": "A", "tokens": 1},
      {"task": "B", "tokens": 1}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// Two started jobs with deadlines 10 (X) and 8 (Y). At release both release
// clocks are zero, so only Y is earliest. Test 4 relaxes r_X to [0,4], which
// makes both total orders feasible and exercises both branch constraints.
const char* kBothOrdersJson = R"({
  "graph": {"name": "EdfBothOrders"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "edf",
    "start": [
      {"task": "X", "tokens": 1},
      {"task": "Y", "tokens": 1}
    ]
  },
  "nodes": [
    {"id": "X", "type": "task", "priority": 0, "core": 0, "deadline": 10,
     "time": [[1, 1]], "locks": []},
    {"id": "Y", "type": "task", "priority": 0, "core": 0, "deadline": 8,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// A single periodic task whose execution [3,3] exceeds its period 2. The
// second release necessarily fires while the first job is mid-chain, which the
// supported class rejects rather than queueing a second job.
const char* kOverlappingReleaseJson = R"({
  "graph": {"name": "EdfOverlappingRelease"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "edf",
    "periodic": [{"task": "A", "period": 2}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 4,
     "time": [[3, 3]], "locks": []}
  ],
  "edges": []
})";

// A one-shot task with a positive deadline but neither a period nor a start.
const char* kNotReleasedJson = R"({
  "graph": {"name": "EdfNotReleased"},
  "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "edf"},
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 5,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

const char* kFixedOneShotJson = R"({
  "graph": {"name": "FixedOneShot"},
  "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fixed"},
  "nodes": [
    {"id": "A", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// ---------------------------------------------------------------------------
// 1. Known EDF behavior / WCRT
// ---------------------------------------------------------------------------

TEST(EdfTest, TwoPeriodicTasksHaveKnownEdfResponseTimes) {
  const petri::PTPN net = lower(parse_tdg(kTwoPeriodicJson));
  ASSERT_EQ(net.schedule_policy(), SchedulePolicy::EDF);

  const MetricsReport report = analyze(net);
  const TaskMetrics& a = task_metrics(report, "A");
  const TaskMetrics& b = task_metrics(report, "B");

  ASSERT_TRUE(a.observed);
  ASSERT_TRUE(b.observed);
  EXPECT_FALSE(a.wcrt.infinite);
  EXPECT_FALSE(b.wcrt.infinite);
  EXPECT_EQ(a.wcrt.value, 3);
  EXPECT_EQ(b.wcrt.value, 1);
  EXPECT_EQ(a.wcrt_lb.value, 3);
  EXPECT_EQ(b.wcrt_lb.value, 1);
  EXPECT_FALSE(a.deadline_missed);
  EXPECT_FALSE(b.deadline_missed);
  EXPECT_TRUE(report.schedulable);
}

// ---------------------------------------------------------------------------
// 2. EDF differs from RM/DM through absolute (not relative) deadlines
// ---------------------------------------------------------------------------

TEST(EdfTest, EdfDiffersFromRmAndDmWhenAbsoluteDeadlinesDisagree) {
  const petri::PTPN edf_net = lower(parse_tdg(edf_vs_static_json("edf")));
  const petri::PTPN rm_net = lower(parse_tdg(edf_vs_static_json("rm")));
  const petri::PTPN dm_net = lower(parse_tdg(edf_vs_static_json("dm")));

  const MetricsReport edf_report = analyze(edf_net);
  const MetricsReport rm_report = analyze(rm_net);
  const MetricsReport dm_report = analyze(dm_net);

  const TaskMetrics& edf_a = task_metrics(edf_report, "A");
  const TaskMetrics& edf_b = task_metrics(edf_report, "B");
  const TaskMetrics& rm_a = task_metrics(rm_report, "A");
  const TaskMetrics& rm_b = task_metrics(rm_report, "B");
  const TaskMetrics& dm_a = task_metrics(dm_report, "A");
  const TaskMetrics& dm_b = task_metrics(dm_report, "B");

  // EDF keeps the running one-shot job at t=5 because its absolute deadline 8
  // is earlier than the newly released periodic job's deadline 10.
  EXPECT_EQ(edf_b.wcrt.value, 6);
  EXPECT_EQ(edf_a.wcrt.value, 2);

  // Both static policies rank A above B; A preempts immediately at 5.
  EXPECT_EQ(rm_b.wcrt.value, 7);
  EXPECT_EQ(rm_a.wcrt.value, 1);
  EXPECT_EQ(dm_b.wcrt.value, 7);
  EXPECT_EQ(dm_a.wcrt.value, 1);

  EXPECT_NE(edf_b.wcrt.value, rm_b.wcrt.value);
  EXPECT_NE(edf_b.wcrt.value, dm_b.wcrt.value);

  // The EDF graph must actually carry the deadline-order constraint that
  // selected B over A: r_A - r_B <= D_A - D_B = -3 while both are in flight.
  StateClassReachabilityGraph graph(edf_net);
  graph.build(50000);
  ASSERT_FALSE(graph.get_statistics().truncated);
  const std::vector<TaskChain> tasks = state_class::build_task_chains(edf_net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);

  bool found_order_constraint = false;
  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, graph.get_graph(), *it);
    const int r_a = state.release_index(ordinal_a);
    const int r_b = state.release_index(ordinal_b);
    if (r_a <= 0 || r_b <= 0) {
      continue;
    }
    if (state.zone.get_constraint(static_cast<size_t>(r_a), static_cast<size_t>(r_b)) <= -3) {
      found_order_constraint = true;
      break;
    }
  }
  EXPECT_TRUE(found_order_constraint);
}

// ---------------------------------------------------------------------------
// 3. Equal deadlines enumerate every tie
// ---------------------------------------------------------------------------

TEST(EdfTest, EqualDeadlinesEnumerateBothTieBranches) {
  const petri::PTPN net = lower(parse_tdg(kEqualDeadlineJson));
  StateClassReachabilityGraph graph(net);

  std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 2u);

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);
  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t b_get_core = chain_transition(net, "B", 1);
  ASSERT_NE(a_get_core, SIZE_MAX);
  ASSERT_NE(b_get_core, SIZE_MAX);

  int a_first = 0;
  int b_first = 0;
  for (const StateClass& state : initials) {
    ASSERT_EQ(state.zone.size(), state.clock_vars.size());
    ASSERT_EQ(state.release_clock_of_task.size(), tasks.size());
    EXPECT_TRUE(state.has_release_clock(ordinal_a));
    EXPECT_TRUE(state.has_release_clock(ordinal_b));
    const bool picks_a = state_class::contains(state.priority_enabled, a_get_core);
    const bool picks_b = state_class::contains(state.priority_enabled, b_get_core);
    EXPECT_NE(picks_a, picks_b);
    if (picks_a) {
      ++a_first;
    }
    if (picks_b) {
      ++b_first;
    }
  }
  EXPECT_EQ(a_first, 1);
  EXPECT_EQ(b_first, 1);

  // Calling the rule directly exposes the two (redundant at r=0) tie
  // constraints. Both tasks start at time 0, so both constraint sets consist of
  // r_other - r_candidate <= 0.
  EdfFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(initials.front().struct_enabled, net, PolicyContext{net, &initials.front()});
  ASSERT_EQ(branches.size(), 2u);
  ASSERT_EQ(branches[0].constraints.size(), 1u);
  ASSERT_EQ(branches[1].constraints.size(), 1u);
  const BranchConstraint first = branches[0].constraints.front();
  const BranchConstraint second = branches[1].constraints.front();
  EXPECT_EQ(first.bound, 0);
  EXPECT_EQ(second.bound, 0);
  EXPECT_NE(first.left_task, first.right_task);
  EXPECT_EQ(first.left_task, second.right_task);
  EXPECT_EQ(first.right_task, second.left_task);
}

// ---------------------------------------------------------------------------
// 4. Both deadline orders feasible in one zone
// ---------------------------------------------------------------------------

TEST(EdfTest, BothOrdersFeasibleEmitBothConstraintSets) {
  const petri::PTPN net = lower(parse_tdg(kBothOrdersJson));
  StateClassReachabilityGraph graph(net);
  std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 1u);  // at r_X = r_Y = 0 only Y can be earliest

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_x = task_ordinal(tasks, "X");
  const size_t ordinal_y = task_ordinal(tasks, "Y");
  ASSERT_NE(ordinal_x, SIZE_MAX);
  ASSERT_NE(ordinal_y, SIZE_MAX);
  ASSERT_EQ(tasks[ordinal_x].deadline, 10);
  ASSERT_EQ(tasks[ordinal_y].deadline, 8);
  const size_t x_get_core = chain_transition(net, "X", 1);
  const size_t y_get_core = chain_transition(net, "Y", 1);
  ASSERT_NE(x_get_core, SIZE_MAX);
  ASSERT_NE(y_get_core, SIZE_MAX);

  StateClass relaxed = initials.front();
  const int r_x = relaxed.release_index(ordinal_x);
  const int r_y = relaxed.release_index(ordinal_y);
  ASSERT_GT(r_x, 0);
  ASSERT_GT(r_y, 0);

  // r_X is allowed to range over [0,4]; r_Y stays 0. Then:
  //   X earliest iff r_Y - r_X <= -2 (i.e. r_X >= 2),
  //   Y earliest iff r_X - r_Y <=  2 (i.e. r_X <= 2).
  // Both are feasible, and they overlap exactly at the tie r_X = 2.
  //
  // The initial zone pins every clock to zero, so relaxing only r_X <= x0 + 4
  // is not enough: the closure still equates r_X with the frozen execution
  // clocks. Also relax r_X against every other clock; the lower bound r_X >= 0
  // remains through x0.
  const size_t r_x_index = static_cast<size_t>(r_x);
  relaxed.zone.set_constraint(r_x_index, 0, 4);
  for (size_t j = 1; j < relaxed.zone.size(); ++j) {
    if (j != r_x_index) {
      relaxed.zone.set_constraint(r_x_index, j, 4);
    }
  }
  relaxed.zone.minimize();
  ASSERT_TRUE(relaxed.zone.is_consistent());
  EXPECT_EQ(relaxed.zone.get_constraint(0, r_x_index), 0);  // r_X >= 0
  EXPECT_EQ(relaxed.zone.get_constraint(r_x_index, 0), 4);  // r_X <= 4

  EdfFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(relaxed.struct_enabled, net, PolicyContext{net, &relaxed});
  ASSERT_EQ(branches.size(), 2u);

  const auto find_branch = [&](size_t get_core) -> const PolicyBranch* {
    for (const PolicyBranch& branch : branches) {
      if (state_class::contains(branch.active, get_core)) {
        return &branch;
      }
    }
    return nullptr;
  };

  const PolicyBranch* x_branch = find_branch(x_get_core);
  const PolicyBranch* y_branch = find_branch(y_get_core);
  ASSERT_NE(x_branch, nullptr);
  ASSERT_NE(y_branch, nullptr);
  EXPECT_FALSE(state_class::contains(x_branch->active, y_get_core));
  EXPECT_FALSE(state_class::contains(y_branch->active, x_get_core));

  ASSERT_EQ(x_branch->constraints.size(), 1u);
  EXPECT_EQ(x_branch->constraints.front().left_task, ordinal_x);
  EXPECT_EQ(x_branch->constraints.front().right_task, ordinal_y);
  EXPECT_EQ(x_branch->constraints.front().bound, -2);
  ASSERT_EQ(y_branch->constraints.size(), 1u);
  EXPECT_EQ(y_branch->constraints.front().left_task, ordinal_y);
  EXPECT_EQ(y_branch->constraints.front().right_task, ordinal_x);
  EXPECT_EQ(y_branch->constraints.front().bound, 2);

  // Both branch constraints are feasible in the relaxed zone and select the
  // intended ordering when intersected.
  state_class::DBM x_zone = relaxed.zone;
  EXPECT_TRUE(x_zone.tighten(static_cast<size_t>(r_y), static_cast<size_t>(r_x), -2));
  EXPECT_EQ(x_zone.get_constraint(static_cast<size_t>(r_y), static_cast<size_t>(r_x)), -2);
  state_class::DBM y_zone = relaxed.zone;
  EXPECT_TRUE(y_zone.tighten(static_cast<size_t>(r_x), static_cast<size_t>(r_y), 2));
  EXPECT_EQ(y_zone.get_constraint(static_cast<size_t>(r_x), static_cast<size_t>(r_y)), 2);
}

// ---------------------------------------------------------------------------
// 5. Unsupported models are rejected with clear errors
// ---------------------------------------------------------------------------

TEST(EdfTest, RejectsUnsupportedLoweredModels) {
  // A statically partitioned task outside the declared core range. (Multi-core
  // EDF itself is supported; the per-core behavior is covered by
  // test_edf_multicore.cpp.)
  expect_rejected(R"({
    "graph": {"name": "EdfCoreOutOfRange"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "edf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 3,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "outside [0, 2)");

  // Capacity above one (queued jobs are outside the supported class).
  expect_rejected(R"({
    "graph": {"name": "EdfCapacityTwo"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "edf",
                      "task_place_capacity": 2,
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "task_place_capacity == 1");

  // Locks.
  expect_rejected(R"({
    "graph": {"name": "EdfLocks"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "edf",
                      "shared_locks": ["m"],
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": ["m"]}],
    "edges": []
  })",
                  "does not include locks");

  // Dependency-driven release (task-to-task edge).
  expect_rejected(R"({
    "graph": {"name": "EdfDependency"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "edf",
                      "periodic": [{"task": "A", "period": 10},
                                   {"task": "B", "period": 10}]},
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0,
       "time": [[1, 1]], "locks": []},
      {"id": "B", "type": "task", "priority": 0, "core": 0,
       "time": [[1, 1]], "locks": []}
    ],
    "edges": [{"source": "A", "target": "B", "label": "10"}]
  })",
                  "dependency-driven releases");

  // Positive deadline but no release at all.
  expect_rejected(kNotReleasedJson, "neither periodic");

  // No positive deadline and no period.
  expect_rejected(R"({
    "graph": {"name": "EdfNoDeadline"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "edf"},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "positive absolute deadline or period");

  // More than one initial job for a task.
  expect_rejected(R"({
    "graph": {"name": "EdfTwoStarts"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "edf",
                      "start": [{"task": "A", "tokens": 2}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "at most one initial job");

  // A periodic release that overlaps a mid-chain job is rejected at the
  // successor state instead of being queued into the single job slot.
  const petri::PTPN overlap_net = lower(parse_tdg(kOverlappingReleaseJson));
  StateClassReachabilityGraph overlap_graph(overlap_net);
  try {
    overlap_graph.build(10000);
    FAIL() << "expected the overlapping release to be rejected";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("one job per task"), std::string::npos)
        << "unexpected message: " << error.what();
  }
}

TEST(EdfTest, RejectsHandBuiltAndAmbiguousConfiguration) {
  // No task metadata/chain.
  petri::PTPN hand_built;
  hand_built.add_transition("t", petri::TimeInterval(0, 0), 1, 0, false);
  try {
    EdfFiringRule rule(hand_built);
    FAIL() << "expected EDF to reject a net without task chains";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("no task chains"), std::string::npos)
        << "unexpected message: " << error.what();
  }

  // EDF has no scalar priority; a caller cannot silently fall back to one.
  EdfFiringRule unindexed;
  const PolicyContext context{hand_built, nullptr};
  EXPECT_THROW(unindexed.effective_priority(context, 0), std::logic_error);

  // k-extrapolation would relax the release-clock bounds.
  const petri::PTPN edf_net = lower(parse_tdg(kTwoPeriodicJson));
  StateClassReachabilityGraph edf_graph(edf_net);
  EXPECT_THROW(edf_graph.set_extrapolation(true), std::invalid_argument);

  // ... and installing EDF after enabling extrapolation is rejected too.
  const petri::PTPN fixed_net = lower(parse_tdg(kFixedOneShotJson));
  StateClassReachabilityGraph fixed_graph(fixed_net);
  fixed_graph.set_extrapolation(true);
  EXPECT_THROW(fixed_graph.set_firing_rule(std::make_unique<EdfFiringRule>()),
               std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Default (static) firing rules carry no release-clock branches
// ---------------------------------------------------------------------------

TEST(EdfTest, StaticRulesKeepEmptyBranchConstraints) {
  const petri::PTPN net = lower(parse_tdg(kFixedOneShotJson));
  std::unique_ptr<state_class::FiringRule> rule =
      state_class::make_firing_rule(SchedulePolicy::FIXED, net);
  ASSERT_NE(rule, nullptr);
  EXPECT_FALSE(rule->uses_release_clocks());

  const state_class::TransitionSet enabled =
      state_class::Scheduling::structural_enabled(net, net.get_marking());
  const PolicyContext context{net, nullptr};
  const std::vector<PolicyBranch> branches = rule->branches(enabled, net, context);
  ASSERT_FALSE(branches.empty());
  for (const PolicyBranch& branch : branches) {
    EXPECT_TRUE(branch.constraints.empty());
  }
}

// ---------------------------------------------------------------------------
// 7. Release-clock layout / zone-dimension invariants
// ---------------------------------------------------------------------------

TEST(EdfTest, ReleaseClockLayoutInvariantsHoldOnEveryState) {
  const petri::PTPN net = lower(parse_tdg(kTwoPeriodicJson));
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  ASSERT_EQ(tasks.size(), 2u);

  StateClassReachabilityGraph graph(net);
  graph.build(50000);
  ASSERT_FALSE(graph.get_statistics().truncated);
  ASSERT_GT(graph.get_statistics().total_states, 1u);

  bool saw_two_in_flight = false;
  bool saw_no_in_flight = false;

  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, graph.get_graph(), *it);

    ASSERT_EQ(state.zone.size(), state.clock_vars.size());
    ASSERT_EQ(state.release_clock_of_task.size(), tasks.size());
    ASSERT_EQ(state.exec_clock_of_transition.size(), net.num_transitions());
    ASSERT_EQ(state.susp_clock_of_transition.size(), net.num_transitions());
    ASSERT_FALSE(state.clock_vars.empty());
    EXPECT_EQ(state.clock_vars.front().kind, ClockKind::Zero);

    // The task chain never carries more than the one supported job. (A
    // completed job's exit token is still counted here, so the bound is 1 only
    // while the consumer is pending; the in-flight count is separate.)
    std::vector<size_t> in_flight_ordinals;
    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      const bool in_flight = state_class::task_in_flight(tasks[ordinal], state.marking);
      const int release_index = state.release_index(ordinal);
      if (in_flight) {
        in_flight_ordinals.push_back(ordinal);
        ASSERT_GT(release_index, 0) << tasks[ordinal].name;
        ASSERT_LT(static_cast<size_t>(release_index), state.clock_vars.size());
        EXPECT_EQ(state.clock_vars[static_cast<size_t>(release_index)].kind, ClockKind::Release);
        EXPECT_EQ(state.clock_vars[static_cast<size_t>(release_index)].task, ordinal);
        EXPECT_EQ(state.clock_vars[static_cast<size_t>(release_index)].transition,
                  state_class::NO_TRANSITION);
      } else {
        EXPECT_EQ(release_index, -1) << tasks[ordinal].name;
      }
      EXPECT_LE(state_class::task_chain_token_count(tasks[ordinal], state.marking), 1u);
    }

    // Release clocks form a prefix immediately after x0, in ascending task
    // ordinal order among the in-flight tasks, independent of the selected
    // active set.
    for (size_t i = 1; i <= in_flight_ordinals.size(); ++i) {
      ASSERT_LT(i, state.clock_vars.size());
      EXPECT_EQ(state.clock_vars[i].kind, ClockKind::Release);
      EXPECT_EQ(state.clock_vars[i].task, in_flight_ordinals[i - 1]);
    }
    for (size_t i = in_flight_ordinals.size() + 1; i < state.clock_vars.size(); ++i) {
      EXPECT_NE(state.clock_vars[i].kind, ClockKind::Release);
    }

    // Rebuilding the layout from the marking must reproduce it exactly.
    StateClass rebuilt = state;
    state_class::build_clock_layout(rebuilt, net, /*keep_suspension_clocks=*/false,
                                    /*include_release_clocks=*/true);
    ASSERT_EQ(rebuilt.clock_vars.size(), state.clock_vars.size());
    for (size_t i = 0; i < rebuilt.clock_vars.size(); ++i) {
      EXPECT_EQ(rebuilt.clock_vars[i].kind, state.clock_vars[i].kind) << i;
      EXPECT_EQ(rebuilt.clock_vars[i].transition, state.clock_vars[i].transition) << i;
      EXPECT_EQ(rebuilt.clock_vars[i].task, state.clock_vars[i].task) << i;
    }
    EXPECT_EQ(rebuilt.release_clock_of_task, state.release_clock_of_task);
    EXPECT_EQ(rebuilt.exec_clock_of_transition, state.exec_clock_of_transition);

    saw_two_in_flight = saw_two_in_flight || in_flight_ordinals.size() == 2;
    saw_no_in_flight = saw_no_in_flight || in_flight_ordinals.empty();
  }

  EXPECT_TRUE(saw_two_in_flight);
  EXPECT_TRUE(saw_no_in_flight);
}

}  // namespace
