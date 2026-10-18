#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/dbm/clock_state.h"
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
using state_class::contains;
using state_class::DBM;
using state_class::INF_TIME;
using state_class::LlfFiringRule;
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

MetricsReport analyze(const petri::PTPN& net, size_t max_states = 200000) {
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

// Exact chi bounds [lower, upper] of `ordinal` in `state`.
struct LaxityBounds {
  int lower = 0;
  int upper = 0;
  bool finite = false;
};

LaxityBounds laxity_bounds(const StateClass& state, size_t ordinal) {
  const int index = state.laxity_index(ordinal);
  if (index <= 0) {
    return {};
  }
  const int lower_raw = state.zone.get_constraint(0, static_cast<size_t>(index));
  const int upper = state.zone.get_constraint(static_cast<size_t>(index), 0);
  return LaxityBounds{lower_raw == INF_TIME ? 0 : -lower_raw, upper,
                      lower_raw != INF_TIME && upper != INF_TIME};
}

bool chi_exact(const StateClass& state, size_t ordinal, int value) {
  const LaxityBounds bounds = laxity_bounds(state, ordinal);
  return bounds.finite && bounds.lower == value && bounds.upper == value;
}

// Hand-built standard TDG2PN-style task chain (resume lowering):
//   entry -(get_core,[0,0])-> ready -(exec,[C,C])-> exit
struct HandTask {
  std::string name;
  petri::PlaceId entry{0};
  petri::PlaceId ready{0};
  petri::PlaceId exit{0};
  petri::TransitionId get_core{0};
  petri::TransitionId exec{0};
};

HandTask add_hand_task(petri::PTPN& net, const std::string& name, int wcet, int deadline,
                       int period, bool start_at_zero, int core = 0) {
  HandTask task;
  task.name = name;
  task.entry = net.add_place(name + "_entry", 1, true);
  task.get_core = net.add_transition(name + "_get_core", petri::TimeInterval(0, 0), 0, core, false);
  task.ready = net.add_place(name + "_ready", 1, true);
  task.exec = net.add_transition(name + "_exec", petri::TimeInterval(wcet, wcet), 0, core,
                                 /*suspendable=*/true);
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
  info.period = period;
  info.deadline = deadline;
  net.set_task_info(name, info);
  if (start_at_zero) {
    net.set_initial_marking(task.entry, 1);
  }
  return task;
}

// One-shot delayed release registered as the task's release transition.
void add_one_shot_release(petri::PTPN& net, const HandTask& task, int at) {
  const petri::PlaceId period_place = net.add_place(task.name + "_period", 1, true);
  net.set_initial_marking(period_place, 1);
  const petri::TransitionId fire = net.add_transition(
      task.name + "_fire", petri::TimeInterval(at, at), 0, petri::kControlTransitionCore, false);
  net.set_pre_arc(period_place, fire, 1);
  net.set_post_arc(fire, task.entry, 1);
  net.set_periodic_transition(task.name, fire);
}

// Zero-time completion consumer, as produced by the lowering. Without it the
// exit token is an illegitimate sink that the metrics analyzer reports as a
// deadlock.
void add_end_consumer(petri::PTPN& net, const HandTask& task) {
  const petri::PlaceId done = net.add_place(task.name + "_done", 1, true);
  const petri::TransitionId consume = net.add_transition(
      task.name + "_consume", petri::TimeInterval(0, 0), 0, petri::kControlTransitionCore, false);
  net.set_pre_arc(task.exit, consume, 1);
  net.set_post_arc(consume, done, 1);
}

// Two cores with two one-shot jobs each. The per-core chi orders disagree with
// a global order (B is most urgent on core 0, C on core 1), so per-core LLF
// runs B and C concurrently:
//   t=0: B and C run (1 unit each)
//   t=1: A and D run (1 unit each)
// Response times: A = 2, B = 1, C = 1, D = 2.
const char* kTwoCoresJson = R"({
  "graph": {"name": "LlfTwoCores"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "llf",
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
    {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 2,
     "time": [[1, 1]], "locks": []},
    {"id": "C", "type": "task", "priority": 0, "core": 1, "deadline": 3,
     "time": [[1, 1]], "locks": []},
    {"id": "D", "type": "task", "priority": 0, "core": 1, "deadline": 10,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// Equal deadlines on both cores: every per-core maximum-chi tie is feasible,
// so the Cartesian product is 2 choices on core 0 x 2 choices on core 1 = 4.
const char* kFourWayTieJson = R"({
  "graph": {"name": "LlfFourWayTie"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "llf",
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

// Core 0 has two tied in-flight jobs; core 1 has no in-flight job but its
// periodic release is a structurally enabled control transition.
const char* kIdleCoreJson = R"({
  "graph": {"name": "LlfIdleCore"},
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "policy": "llf",
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

// ---------------------------------------------------------------------------
// 1. Per-core maximum chi, no cross-core ordering, known WCRT values
// ---------------------------------------------------------------------------

TEST(LlfMultiCoreTest, PerCoreMaximumChiProducesKnownResponseTimes) {
  const petri::PTPN net = lower(parse_tdg(kTwoCoresJson));
  ASSERT_EQ(net.schedule_policy(), SchedulePolicy::LLF);
  // LLF is a resume policy: no structural CPU place is installed.
  EXPECT_TRUE(net.cpu_places().empty());

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  ASSERT_EQ(tasks.size(), 4u);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  const size_t ordinal_c = task_ordinal(tasks, "C");
  const size_t ordinal_d = task_ordinal(tasks, "D");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);
  ASSERT_NE(ordinal_c, SIZE_MAX);
  ASSERT_NE(ordinal_d, SIZE_MAX);

  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 1u);
  const StateClass& initial = initials.front();

  // chi = wcet - deadline: A=-4, B=-1, C=-2, D=-9. Core 0 picks B, core 1
  // picks C, and the choices are made independently.
  EXPECT_TRUE(chi_exact(initial, ordinal_a, -4));
  EXPECT_TRUE(chi_exact(initial, ordinal_b, -1));
  EXPECT_TRUE(chi_exact(initial, ordinal_c, -2));
  EXPECT_TRUE(chi_exact(initial, ordinal_d, -9));
  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t b_get_core = chain_transition(net, "B", 1);
  const size_t c_get_core = chain_transition(net, "C", 1);
  const size_t d_get_core = chain_transition(net, "D", 1);
  ASSERT_NE(a_get_core, SIZE_MAX);
  ASSERT_NE(b_get_core, SIZE_MAX);
  ASSERT_NE(c_get_core, SIZE_MAX);
  ASSERT_NE(d_get_core, SIZE_MAX);
  EXPECT_FALSE(contains(initial.priority_enabled, a_get_core));
  EXPECT_TRUE(contains(initial.priority_enabled, b_get_core));
  EXPECT_TRUE(contains(initial.priority_enabled, c_get_core));
  EXPECT_FALSE(contains(initial.priority_enabled, d_get_core));

  const MetricsReport report = analyze(net);
  const TaskMetrics& a = task_metrics(report, "A");
  const TaskMetrics& b = task_metrics(report, "B");
  const TaskMetrics& c = task_metrics(report, "C");
  const TaskMetrics& d = task_metrics(report, "D");

  // Both cores progress in parallel; a global chi order would serialize them.
  EXPECT_EQ(a.wcrt.value, 2);
  EXPECT_EQ(b.wcrt.value, 1);
  EXPECT_EQ(c.wcrt.value, 1);
  EXPECT_EQ(d.wcrt.value, 2);
  EXPECT_FALSE(a.wcrt.infinite);
  EXPECT_FALSE(b.wcrt.infinite);
  EXPECT_FALSE(c.wcrt.infinite);
  EXPECT_FALSE(d.wcrt.infinite);
  EXPECT_TRUE(report.schedulable);
}

// ---------------------------------------------------------------------------
// 2. Per-core maximum-chi ties enumerate the Cartesian product
// ---------------------------------------------------------------------------

TEST(LlfMultiCoreTest, PerCoreTiesEnumerateCartesianProduct) {
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

  LlfFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(initials.front().struct_enabled, net, PolicyContext{net, &initials.front()});
  ASSERT_EQ(branches.size(), 4u);
  for (const PolicyBranch& branch : branches) {
    EXPECT_EQ(branch.active.size(), 2u);       // one get_core per core
    EXPECT_EQ(branch.constraints.size(), 2u);  // one same-core pair per core
    for (const BranchConstraint& constraint : branch.constraints) {
      EXPECT_EQ(constraint.domain, ClockKind::Laxity);
      EXPECT_EQ(constraint.bound, 0);
    }
  }
}

// ---------------------------------------------------------------------------
// 3. Urgency orders are local to a core and independent across cores
// ---------------------------------------------------------------------------

TEST(LlfMultiCoreTest, UrgencyOrdersAreIndependentAcrossCores) {
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

  // Pin chi directly: A and B tie for maximum urgency on core 0; C is strictly
  // more urgent than D on core 1.
  StateClass pinned = initials.front();
  DBM zone(pinned.zone.size());
  const auto pin = [&](size_t ordinal, int value) {
    const int index = pinned.laxity_index(ordinal);
    ASSERT_GT(index, 0);
    zone.set_constraint(static_cast<size_t>(index), 0, value);
    zone.set_constraint(0, static_cast<size_t>(index), -value);
  };
  pin(ordinal_a, -4);
  pin(ordinal_b, -4);
  pin(ordinal_c, -2);
  pin(ordinal_d, -9);
  zone.minimize();
  ASSERT_TRUE(zone.is_consistent());
  pinned.zone = zone;

  LlfFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(pinned.struct_enabled, net, PolicyContext{net, &pinned});
  ASSERT_EQ(branches.size(), 2u);  // {A, C} and {B, C}

  int a_branches = 0;
  int b_branches = 0;
  for (const PolicyBranch& branch : branches) {
    const bool picks_a = contains(branch.active, a_get_core);
    const bool picks_b = contains(branch.active, b_get_core);
    EXPECT_NE(picks_a, picks_b);
    // Core 1 always picks C: the core-0 tie did not leak across cores.
    EXPECT_TRUE(contains(branch.active, c_get_core));
    EXPECT_FALSE(contains(branch.active, d_get_core));
    a_branches += picks_a ? 1 : 0;
    b_branches += picks_b ? 1 : 0;

    ASSERT_EQ(branch.constraints.size(), 2u);  // one pair per core
    bool saw_core0 = false;
    bool saw_core1 = false;
    for (const BranchConstraint& constraint : branch.constraints) {
      EXPECT_EQ(constraint.domain, ClockKind::Laxity);
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
// 4. Direct branch check: no cross-core chi comparisons
// ---------------------------------------------------------------------------

TEST(LlfMultiCoreTest, BranchConstraintsNeverReferenceCrossCorePairs) {
  const petri::PTPN net = lower(parse_tdg(kFourWayTieJson));
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);

  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_FALSE(initials.empty());

  LlfFiringRule rule(net);
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
        EXPECT_EQ(constraint.domain, ClockKind::Laxity);
        EXPECT_EQ(constraint.bound, 0);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 5. Maximum-chi preemption on one core leaves another core unaffected
// ---------------------------------------------------------------------------

TEST(LlfMultiCoreTest, PreemptionIsPerCoreAndDoesNotDelayOtherCores) {
  petri::PTPN net;
  // Core 0: L runs 0..2, is preempted by H's release at 2, resumes 3..11.
  const HandTask l = add_hand_task(net, "L", 10, 30, 0, /*start_at_zero=*/true, 0);
  const HandTask h = add_hand_task(net, "H", 1, 3, 2, /*start_at_zero=*/false, 0);
  add_one_shot_release(net, h, 2);
  // Core 1: independent urgent and lax one-shot jobs.
  const HandTask x = add_hand_task(net, "X", 1, 2, 0, /*start_at_zero=*/true, 1);
  const HandTask y = add_hand_task(net, "Y", 1, 5, 0, /*start_at_zero=*/true, 1);
  for (const HandTask& task : {l, h, x, y}) {
    add_end_consumer(net, task);
  }
  net.set_schedule_policy(SchedulePolicy::LLF);

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_l = task_ordinal(tasks, "L");
  const size_t ordinal_h = task_ordinal(tasks, "H");
  ASSERT_NE(ordinal_l, SIZE_MAX);
  ASSERT_NE(ordinal_h, SIZE_MAX);
  const size_t l_exec = chain_transition(net, "L", 3);
  const size_t h_exec = chain_transition(net, "H", 3);
  ASSERT_NE(l_exec, SIZE_MAX);
  ASSERT_NE(h_exec, SIZE_MAX);

  const MetricsReport report = analyze(net);
  // H (chi = 1-3 = -2 at release) preempts L (chi = 10-30 = -20): L loses the
  // 2 units before the release and the 1-unit H run, finishing at 11.
  EXPECT_EQ(task_metrics(report, "L").wcrt.value, 11);
  EXPECT_EQ(task_metrics(report, "H").wcrt.value, 1);
  // Core 1 keeps its own urgency order (X before Y) and is not delayed.
  EXPECT_EQ(task_metrics(report, "X").wcrt.value, 1);
  EXPECT_EQ(task_metrics(report, "Y").wcrt.value, 2);
  EXPECT_TRUE(report.schedulable);

  StateClassReachabilityGraph graph(net);
  graph.build(200000);
  ASSERT_FALSE(graph.get_statistics().truncated);
  bool saw_preemption = false;
  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, graph.get_graph(), *it);
    if (!contains(state.priority_enabled, h_exec) ||
        !state_class::task_in_flight(tasks[ordinal_l], state.marking) ||
        !state_class::task_in_flight(tasks[ordinal_h], state.marking)) {
      continue;
    }
    saw_preemption = true;
    // H has maximum chi on core 0; the running L segment is suspended.
    EXPECT_TRUE(chi_exact(state, ordinal_h, -2));
    EXPECT_TRUE(chi_exact(state, ordinal_l, -20));
    EXPECT_TRUE(contains(state.suspended, l_exec));
  }
  EXPECT_TRUE(saw_preemption);
}

// ---------------------------------------------------------------------------
// 6. An idle core's control transition combines with the busy core choice
// ---------------------------------------------------------------------------

TEST(LlfMultiCoreTest, IdleCoreControlTransitionsCombineWithBusyCoreChoice) {
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
// 7. Multi-core guards
// ---------------------------------------------------------------------------

TEST(LlfMultiCoreTest, RejectsUnsupportedMultiCoreModels) {
  // num_cpus below one.
  expect_rejected(R"({
    "graph": {"name": "LlfNoCpus"},
    "configuration": {"num_cpus": 0, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "num_cpus >= 1");

  // Task core outside the declared core range.
  expect_rejected(R"({
    "graph": {"name": "LlfCoreOutOfRange"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 2,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "outside [0, 2)");

  // More than one core per CPU (per-core parallelism is not modeled).
  expect_rejected(R"({
    "graph": {"name": "LlfCoresPerCpu"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 2, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "cores_per_cpu == 1");

  // Locks.
  expect_rejected(R"({
    "graph": {"name": "LlfLocksMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
                      "shared_locks": ["m"],
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": ["m"]}],
    "edges": []
  })",
                  "does not include locks");

  // Fork/join.
  expect_rejected(R"({
    "graph": {"name": "LlfForkMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0,
       "deadline": 5, "time": [[1, 1]], "locks": []},
      {"id": "F", "type": "fork", "core": 0, "time": [[1, 1]]}
    ],
    "edges": []
  })",
                  "fork/join");

  // Dependency-driven release between tasks on different cores.
  expect_rejected(R"({
    "graph": {"name": "LlfDependencyMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
                      "periodic": [{"task": "A", "period": 10},
                                   {"task": "B", "period": 10}]},
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 5,
       "time": [[1, 1]], "locks": []},
      {"id": "B", "type": "task", "priority": 0, "core": 1, "deadline": 5,
       "time": [[1, 1]], "locks": []}
    ],
    "edges": [{"source": "A", "target": "B", "label": "10"}]
  })",
                  "dependency-driven releases");

  // Capacity above one (queued jobs are outside the supported class).
  expect_rejected(R"({
    "graph": {"name": "LlfCapacityTwoMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
                      "task_place_capacity": 2,
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "task_place_capacity == 1");

  // More than one initial job for a task.
  expect_rejected(R"({
    "graph": {"name": "LlfTwoStartsMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 2}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "at most one initial job");

  // Missing deadline (no positive absolute deadline or period).
  expect_rejected(R"({
    "graph": {"name": "LlfNoDeadline"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "positive absolute deadline or period");

  // Non-deterministic execution segment.
  expect_rejected(R"({
    "graph": {"name": "LlfNondeterministic"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 2]], "locks": []}],
    "edges": []
  })",
                  "deterministic execution segments");

  // A periodic release that overlaps a mid-chain job is rejected at the
  // successor state instead of being queued into the single job slot.
  const petri::PTPN overlap_net = lower(parse_tdg(R"({
    "graph": {"name": "LlfOverlapMultiCore"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "llf",
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

// The rule itself rejects a chain transition whose core disagrees with
// TaskInfo, even when the lowering validator is bypassed.
TEST(LlfMultiCoreTest, RejectsChainTransitionOnWrongCore) {
  petri::PTPN net;
  const petri::PlaceId entry = net.add_place("A_entry", 1, true);
  const petri::TransitionId get_core =
      net.add_transition("A_get_core", petri::TimeInterval(0, 0), 0, 1, false);
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
  info.core = 0;  // transitions are on core 1
  info.wcet = 1;
  info.deadline = 5;
  net.set_task_info("A", info);
  net.set_schedule_policy(SchedulePolicy::LLF);

  try {
    LlfFiringRule rule(net);
    FAIL() << "expected LLF to reject a chain transition on the wrong core";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("chain transition"), std::string::npos)
        << "unexpected message: " << error.what();
  }
}

}  // namespace
