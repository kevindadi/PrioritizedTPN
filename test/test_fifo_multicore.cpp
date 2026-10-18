#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
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

namespace {

using state_class::BranchConstraint;
using state_class::ClockKind;
using state_class::DBM;
using state_class::FifoFiringRule;
using state_class::MetricsReport;
using state_class::PolicyBranch;
using state_class::PolicyContext;
using state_class::StateClass;
using state_class::StateClassReachabilityGraph;
using state_class::TaskChain;
using state_class::TaskMetrics;
using state_class::contains;

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

// Hand-built, standard non-preemptive task chain without a structural CPU
// place:
//   entry -(get_core,[0,0])-> ready -(exec,[C,C])-> exit
struct HandTask {
  std::string name;
  petri::PlaceId entry{0};
  petri::PlaceId ready{0};
  petri::PlaceId exit{0};
  petri::TransitionId get_core{0};
  petri::TransitionId exec{0};
};

HandTask add_hand_task(petri::PTPN& net, const std::string& name, int wcet, int core) {
  HandTask task;
  task.name = name;
  task.entry = net.add_place(name + "_entry", 1, true);
  task.get_core =
      net.add_transition(name + "_get_core", petri::TimeInterval(0, 0), 0, core, false);
  task.ready = net.add_place(name + "_ready", 1, true);
  task.exec =
      net.add_transition(name + "_exec", petri::TimeInterval(wcet, wcet), 0, core, false);
  task.exit = net.add_place(name + "_exit", 1, true);
  net.set_pre_arc(task.entry, task.get_core, 1);
  net.set_post_arc(task.get_core, task.ready, 1);
  net.set_pre_arc(task.ready, task.exec, 1);
  net.set_post_arc(task.exec, task.exit, 1);
  net.set_task_chain(name, {petri::NodeRef::of(task.entry), petri::NodeRef::of(task.get_core),
                            petri::NodeRef::of(task.ready), petri::NodeRef::of(task.exec),
                            petri::NodeRef::of(task.exit)});
  petri::TaskInfo info;
  info.core = core;
  info.priority = 0;
  info.wcet = wcet;
  info.bcet = wcet;
  info.deadline = 10;
  net.set_task_info(name, info);
  net.set_initial_marking(task.entry, 1);
  return task;
}

// Two statically partitioned cores, each with a long one-shot job and a
// periodic arrival:
//   core 0: L0 [6,6] at 0; A releases at 4 and waits for L0, response 3.
//   core 1: L1 [2,2] at 0; B releases at 2, response 1.
// L0 response 6 and L1 response 2. A single global FIFO queue would serialize
// the cores and inflate these values.
const char* kTwoCoresJson = R"({
  "graph": {"name": "FifoTwoCores"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "fifo",
    "start": [
      {"task": "L0", "tokens": 1},
      {"task": "L1", "tokens": 1}
    ],
    "periodic": [
      {"task": "A", "period": 4},
      {"task": "B", "period": 2}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []},
    {"id": "L0", "type": "task", "priority": 0, "core": 0,
     "time": [[6, 6]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 1,
     "time": [[1, 1]], "locks": []},
    {"id": "L1", "type": "task", "priority": 0, "core": 1,
     "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

// Two one-shot jobs per core, all released together: every per-core arrival
// tie is feasible, so the Cartesian product is 2 x 2 = 4 branches.
const char* kFourWayTieJson = R"({
  "graph": {"name": "FifoFourWayTie"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "fifo",
    "start": [
      {"task": "A", "tokens": 1},
      {"task": "B", "tokens": 1},
      {"task": "C", "tokens": 1},
      {"task": "D", "tokens": 1}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0,
     "time": [[2, 2]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0,
     "time": [[2, 2]], "locks": []},
    {"id": "C", "type": "task", "priority": 0, "core": 1,
     "time": [[2, 2]], "locks": []},
    {"id": "D", "type": "task", "priority": 0, "core": 1,
     "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

// Core 0 has two tied in-flight jobs; core 1 has no in-flight job but its
// periodic release is a structurally enabled control transition.
const char* kIdleCoreJson = R"({
  "graph": {"name": "FifoIdleCore"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "fifo",
    "start": [
      {"task": "A", "tokens": 1},
      {"task": "B", "tokens": 1}
    ],
    "periodic": [
      {"task": "C", "period": 10}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []},
    {"id": "C", "type": "task", "priority": 0, "core": 1,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// ---------------------------------------------------------------------------
// 1. Per-core FIFO selection, no cross-core serialization, known WCRT values
// ---------------------------------------------------------------------------

TEST(FifoMultiCoreTest, PerCoreFifoProducesKnownResponseTimes) {
  const petri::PTPN net = lower(parse_tdg(kTwoCoresJson));
  ASSERT_EQ(net.schedule_policy(), SchedulePolicy::FIFO);

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  ASSERT_EQ(tasks.size(), 4u);
  EXPECT_EQ(tasks[task_ordinal(tasks, "A")].core, 0);
  EXPECT_EQ(tasks[task_ordinal(tasks, "L0")].core, 0);
  EXPECT_EQ(tasks[task_ordinal(tasks, "B")].core, 1);
  EXPECT_EQ(tasks[task_ordinal(tasks, "L1")].core, 1);

  const MetricsReport report = analyze(net);
  const TaskMetrics& a = task_metrics(report, "A");
  const TaskMetrics& l0 = task_metrics(report, "L0");
  const TaskMetrics& b = task_metrics(report, "B");
  const TaskMetrics& l1 = task_metrics(report, "L1");

  EXPECT_EQ(a.wcrt.value, 3);
  EXPECT_EQ(l0.wcrt.value, 6);
  EXPECT_EQ(b.wcrt.value, 1);
  EXPECT_EQ(l1.wcrt.value, 2);
  EXPECT_FALSE(a.wcrt.infinite);
  EXPECT_FALSE(l0.wcrt.infinite);
  EXPECT_FALSE(b.wcrt.infinite);
  EXPECT_FALSE(l1.wcrt.infinite);
  EXPECT_TRUE(report.schedulable);

  // FIFO is non-preemptive on each core.
  EXPECT_EQ(a.max_preemptions, 0);
  EXPECT_EQ(l0.max_preemptions, 0);
  EXPECT_EQ(b.max_preemptions, 0);
  EXPECT_EQ(l1.max_preemptions, 0);
}

// ---------------------------------------------------------------------------
// 2. Per-core arrival ties enumerate the Cartesian product
// ---------------------------------------------------------------------------

TEST(FifoMultiCoreTest, PerCoreTiesEnumerateCartesianProduct) {
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
    const bool picks_a = contains(state.priority_enabled, a_get_core);
    const bool picks_b = contains(state.priority_enabled, b_get_core);
    const bool picks_c = contains(state.priority_enabled, c_get_core);
    const bool picks_d = contains(state.priority_enabled, d_get_core);
    // Exactly one dispatch choice per core, and the cores are independent.
    EXPECT_NE(picks_a, picks_b);
    EXPECT_NE(picks_c, picks_d);
    core0_a += picks_a ? 1 : 0;
    core0_b += picks_b ? 1 : 0;
    core1_c += picks_c ? 1 : 0;
    core1_d += picks_d ? 1 : 0;
  }
  EXPECT_EQ(core0_a, 2);
  EXPECT_EQ(core0_b, 2);
  EXPECT_EQ(core1_c, 2);
  EXPECT_EQ(core1_d, 2);

  FifoFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(initials.front().struct_enabled, net,
                    PolicyContext{net, &initials.front()});
  ASSERT_EQ(branches.size(), 4u);
  for (const PolicyBranch& branch : branches) {
    EXPECT_EQ(branch.active.size(), 2u);      // one get_core per core
    EXPECT_EQ(branch.constraints.size(), 2u);  // one same-core pair per core
  }
}

// ---------------------------------------------------------------------------
// 3. Arrival orders are local to a core and independent across cores
// ---------------------------------------------------------------------------

TEST(FifoMultiCoreTest, ArrivalOrdersAreIndependentAcrossCores) {
  const petri::PTPN net = lower(parse_tdg(kFourWayTieJson));
  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 4u);

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

  // Pin release clocks: A and B arrive together and earliest on core 0 (both
  // orders feasible); C arrived earlier than D on core 1 (only C feasible).
  // r is the time since release, so a larger value means an earlier arrival.
  StateClass relaxed = initials.front();
  DBM zone(relaxed.zone.size());
  const auto pin = [&](size_t ordinal, int value) {
    const int index = relaxed.release_index(ordinal);
    ASSERT_GT(index, 0);
    zone.set_constraint(static_cast<size_t>(index), 0, value);
    zone.set_constraint(0, static_cast<size_t>(index), -value);
  };
  pin(ordinal_a, 5);
  pin(ordinal_b, 5);
  pin(ordinal_c, 5);
  pin(ordinal_d, 3);
  zone.minimize();
  ASSERT_TRUE(zone.is_consistent());
  relaxed.zone = zone;

  FifoFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(relaxed.struct_enabled, net, PolicyContext{net, &relaxed});
  ASSERT_EQ(branches.size(), 2u);  // {A, C} and {B, C}

  int a_branches = 0;
  int b_branches = 0;
  for (const PolicyBranch& branch : branches) {
    const bool picks_a = contains(branch.active, a_get_core);
    const bool picks_b = contains(branch.active, b_get_core);
    EXPECT_NE(picks_a, picks_b);
    // Core 1 always dispatches C: the core-0 tie did not leak across cores.
    EXPECT_TRUE(contains(branch.active, c_get_core));
    EXPECT_FALSE(contains(branch.active, d_get_core));
    a_branches += picks_a ? 1 : 0;
    b_branches += picks_b ? 1 : 0;

    ASSERT_EQ(branch.constraints.size(), 2u);  // one pair per core
    bool saw_core0 = false;
    bool saw_core1 = false;
    for (const BranchConstraint& constraint : branch.constraints) {
      EXPECT_EQ(constraint.domain, ClockKind::Release);
      EXPECT_EQ(constraint.bound, 0);
      EXPECT_EQ(tasks[constraint.left_task].core, tasks[constraint.right_task].core)
          << "constraint compares tasks on different cores";
      const bool core0_pair =
          (constraint.left_task == ordinal_a && constraint.right_task == ordinal_b) ||
          (constraint.left_task == ordinal_b && constraint.right_task == ordinal_a);
      const bool core1_pair =
          (constraint.left_task == ordinal_c && constraint.right_task == ordinal_d) ||
          (constraint.left_task == ordinal_d && constraint.right_task == ordinal_c);
      EXPECT_TRUE(core0_pair || core1_pair);
      saw_core0 = saw_core0 || core0_pair;
      saw_core1 = saw_core1 || core1_pair;
    }
    EXPECT_TRUE(saw_core0);
    EXPECT_TRUE(saw_core1);
  }
  EXPECT_EQ(a_branches, 1);
  EXPECT_EQ(b_branches, 1);
}

// ---------------------------------------------------------------------------
// 4. An idle core's control transition combines with the busy core choice
// ---------------------------------------------------------------------------

TEST(FifoMultiCoreTest, IdleCoreControlTransitionsCombineWithBusyCoreChoice) {
  const petri::PTPN net = lower(parse_tdg(kIdleCoreJson));
  StateClassReachabilityGraph graph(net);

  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 2u);  // core 0 tie; core 1 contributes no choice

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
    // The global control transition is included exactly once per branch.
    EXPECT_TRUE(contains(state.priority_enabled, c_fire));
    const bool picks_a = contains(state.priority_enabled, a_get_core);
    const bool picks_b = contains(state.priority_enabled, b_get_core);
    EXPECT_NE(picks_a, picks_b);
    // The idle core contributes no chain transition.
    EXPECT_FALSE(contains(state.priority_enabled, c_get_core));
    a_branches += picks_a ? 1 : 0;
    b_branches += picks_b ? 1 : 0;
  }
  EXPECT_EQ(a_branches, 1);
  EXPECT_EQ(b_branches, 1);
}

// ---------------------------------------------------------------------------
// 5. Direct branch check: no cross-core arrival constraints
// ---------------------------------------------------------------------------

TEST(FifoMultiCoreTest, BranchConstraintsNeverReferenceCrossCorePairs) {
  const petri::PTPN net = lower(parse_tdg(kFourWayTieJson));
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);

  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_FALSE(initials.empty());

  FifoFiringRule rule(net);
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
        EXPECT_EQ(constraint.bound, 0);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 6. Rule-level per-core non-preemption without a structural CPU place
// ---------------------------------------------------------------------------

TEST(FifoMultiCoreTest, BusyCoreExcludesPendingDispatchWithoutCpuPlaces) {
  petri::PTPN net;
  const HandTask running = add_hand_task(net, "R", 3, 0);
  const HandTask pending = add_hand_task(net, "P", 1, 0);
  const HandTask core1_a = add_hand_task(net, "C", 1, 1);
  const HandTask core1_b = add_hand_task(net, "D", 1, 1);
  net.set_schedule_policy(SchedulePolicy::FIFO);
  ASSERT_TRUE(net.cpu_places().empty());

  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 4u);  // core 0: R/P tie; core 1: C/D tie

  // Put R mid-chain: it is running and holds core 0. Everything stays in
  // flight, so the release-clock layout (and its indices) is unchanged.
  StateClass busy = initials.front();
  busy.marking[running.entry.index()] = 0;
  busy.marking[running.ready.index()] = 1;
  const state_class::TransitionSet struct_enabled =
      state_class::Scheduling::structural_enabled(net, busy.marking);
  EXPECT_TRUE(contains(struct_enabled, running.exec.index()));
  EXPECT_TRUE(contains(struct_enabled, pending.get_core.index()));  // no CPU place
  EXPECT_TRUE(contains(struct_enabled, core1_a.get_core.index()));
  EXPECT_TRUE(contains(struct_enabled, core1_b.get_core.index()));

  FifoFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(struct_enabled, net, PolicyContext{net, &busy});
  ASSERT_EQ(branches.size(), 2u);  // the independent core-1 tie only

  int c_branches = 0;
  int d_branches = 0;
  for (const PolicyBranch& branch : branches) {
    // Core 0 is non-preemptive: only the running job's chain transition is
    // active, even though the pending job's get_core is structurally enabled.
    EXPECT_TRUE(contains(branch.active, running.exec.index()));
    EXPECT_FALSE(contains(branch.active, pending.get_core.index()));
    const bool picks_c = contains(branch.active, core1_a.get_core.index());
    const bool picks_d = contains(branch.active, core1_b.get_core.index());
    EXPECT_NE(picks_c, picks_d);
    c_branches += picks_c ? 1 : 0;
    d_branches += picks_d ? 1 : 0;
  }
  EXPECT_EQ(c_branches, 1);
  EXPECT_EQ(d_branches, 1);
}

TEST(FifoMultiCoreTest, RejectsTwoRunningJobsOnOneCoreWithoutCpuPlaces) {
  petri::PTPN net;
  const HandTask first = add_hand_task(net, "R1", 2, 0);
  const HandTask second = add_hand_task(net, "R2", 2, 0);
  net.set_schedule_policy(SchedulePolicy::FIFO);
  ASSERT_TRUE(net.cpu_places().empty());

  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_FALSE(initials.empty());

  StateClass two_running = initials.front();
  two_running.marking[first.entry.index()] = 0;
  two_running.marking[first.ready.index()] = 1;
  two_running.marking[second.entry.index()] = 0;
  two_running.marking[second.ready.index()] = 1;
  const state_class::TransitionSet struct_enabled =
      state_class::Scheduling::structural_enabled(net, two_running.marking);

  FifoFiringRule rule(net);
  try {
    (void)rule.branches(struct_enabled, net, PolicyContext{net, &two_running});
    FAIL() << "expected FIFO to reject two running jobs on one core";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("at most one running job per real core"),
              std::string::npos)
        << "unexpected message: " << error.what();
  }
}

// ---------------------------------------------------------------------------
// 7. Multi-core guards
// ---------------------------------------------------------------------------

TEST(FifoMultiCoreTest, RejectsUnsupportedMultiCoreModels) {
  // num_cpus below one.
  expect_rejected(R"({
    "graph": {"name": "FifoNoCpus"},
    "configuration": {"num_cpus": 0, "cores_per_cpu": 1, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                 "num_cpus >= 1");

  // Task core outside the declared core range.
  expect_rejected(R"({
    "graph": {"name": "FifoCoreOutOfRange"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 2,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                 "outside [0, 2)");

  // More than one core per CPU (per-core parallelism is not modeled).
  expect_rejected(R"({
    "graph": {"name": "FifoCoresPerCpu"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 2, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                 "cores_per_cpu == 1");

  // Locks.
  expect_rejected(R"({
    "graph": {"name": "FifoLocksMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fifo",
                      "shared_locks": ["m"],
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": ["m"]}],
    "edges": []
  })",
                 "does not include locks");

  // Fork/join.
  expect_rejected(R"({
    "graph": {"name": "FifoForkMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0,
       "time": [[1, 1]], "locks": []},
      {"id": "F", "type": "fork", "core": 0, "time": [[1, 1]]}
    ],
    "edges": []
  })",
                 "fork/join");

  // Dependency-driven release between tasks on different cores.
  expect_rejected(R"({
    "graph": {"name": "FifoDependencyMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fifo",
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
    "graph": {"name": "FifoCapacityTwoMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fifo",
                      "task_place_capacity": 2,
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                 "task_place_capacity == 1");

  // More than one initial job for a task.
  expect_rejected(R"({
    "graph": {"name": "FifoTwoStartsMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 2}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                 "at most one initial job");

  // A periodic release that overlaps a mid-chain job is rejected at the
  // successor state instead of being queued into the single job slot.
  const petri::PTPN overlap_net = lower(parse_tdg(R"({
    "graph": {"name": "FifoOverlapMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fifo",
                      "periodic": [{"task": "A", "period": 2}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 1,
               "time": [[3, 3]], "locks": []}],
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

// The rule itself rejects a chain transition whose core disagrees with
// TaskInfo, even when the lowering validator is bypassed.
TEST(FifoMultiCoreTest, RejectsChainTransitionOnWrongCore) {
  petri::PTPN net;
  const petri::PlaceId entry = net.add_place("A_entry", 1, true);
  const petri::TransitionId get_core =
      net.add_transition("A_get_core", petri::TimeInterval(0, 0), 0, 1, false);
  const petri::PlaceId ready = net.add_place("A_ready", 1, true);
  const petri::TransitionId exec =
      net.add_transition("A_exec", petri::TimeInterval(1, 1), 0, 1, false);
  const petri::PlaceId exit = net.add_place("A_exit", 1, true);
  net.set_pre_arc(entry, get_core);
  net.set_post_arc(get_core, ready);
  net.set_pre_arc(ready, exec);
  net.set_post_arc(exec, exit);
  net.set_initial_marking(entry, 1);
  net.set_task_chain("A", {petri::NodeRef::of(entry), petri::NodeRef::of(get_core),
                           petri::NodeRef::of(ready), petri::NodeRef::of(exec),
                           petri::NodeRef::of(exit)});
  petri::TaskInfo info;
  info.core = 0;  // transitions are on core 1
  info.wcet = 1;
  net.set_task_info("A", info);
  net.set_schedule_policy(SchedulePolicy::FIFO);

  try {
    FifoFiringRule rule(net);
    FAIL() << "expected FIFO to reject a chain transition on the wrong core";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("chain transition"), std::string::npos)
        << "unexpected message: " << error.what();
  }
}

}  // namespace
