#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/dbm/clock_state.h"
#include "analysis/dbm/dbm.h"
#include "analysis/metrics/metrics.h"
#include "analysis/reachability/canonicalization.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"
#include "analysis/reachability/scheduling_policy.h"
#include "analysis/reachability/state_class.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"

namespace {

using state_class::ClockKind;
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

// Position in the task's `node_pn_map` chain (0 = entry place, 1 = get_core,
// 2 = ready, 3,5,... = execution segments).
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

// Exact laxity bounds [lower, upper] of `ordinal` in `state`.
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

bool task_active(const StateClass& state, const TaskChain& task) {
  for (size_t transition : task.transitions) {
    if (state_class::contains(state.priority_enabled, transition)) {
      return true;
    }
  }
  return false;
}

// Hand-built standard TDG2PN-style task chain:
//   entry -(get_core,[0,0])-> ready -(exec1,[c1,c1])-> p1 -> ... -> exit
// `suspendable` execution segments match the LLF resume lowering.
struct HandTask {
  std::string name;
  petri::PlaceId entry{0};
  petri::PlaceId exit{0};
  std::vector<petri::TransitionId> execs;
};

HandTask add_hand_task(petri::PTPN& net, const std::string& name, const std::vector<int>& segments,
                       int deadline, int period, bool start_at_zero, int core = 0) {
  HandTask task;
  task.name = name;
  task.entry = net.add_place(name + "_entry", 1, true);
  const petri::TransitionId get_core =
      net.add_transition(name + "_get_core", petri::TimeInterval(0, 0), 0, core, false);
  const petri::PlaceId ready = net.add_place(name + "_ready", 1, true);

  std::vector<petri::NodeRef> chain = {petri::NodeRef::of(task.entry), petri::NodeRef::of(get_core),
                                       petri::NodeRef::of(ready)};
  net.set_pre_arc(task.entry, get_core, 1);
  net.set_post_arc(get_core, ready, 1);

  petri::PlaceId previous = ready;
  int wcet = 0;
  for (size_t i = 0; i < segments.size(); ++i) {
    const petri::TransitionId exec = net.add_transition(
        name + "_exec" + std::to_string(i), petri::TimeInterval(segments[i], segments[i]), 0, core,
        /*suspendable=*/true);
    const petri::PlaceId next = net.add_place(name + "_p" + std::to_string(i), 1, true);
    net.set_pre_arc(previous, exec, 1);
    net.set_post_arc(exec, next, 1);
    chain.push_back(petri::NodeRef::of(exec));
    chain.push_back(petri::NodeRef::of(next));
    task.execs.push_back(exec);
    previous = next;
    wcet += segments[i];
  }
  task.exit = previous;
  net.set_task_chain(name, chain);

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

// One-shot delayed release: a unit period token is consumed once by an exact
// [at,at] control transition. Registering it as the task's release transition
// keeps the analysis supported-class checks and the metrics release detection
// identical to a periodic task, without a second (overlapping) release.
void add_one_shot_release(petri::PTPN& net, const HandTask& task, int at) {
  const petri::PlaceId period_place = net.add_place(task.name + "_period", 1, true);
  net.set_initial_marking(period_place, 1);
  const petri::TransitionId fire = net.add_transition(
      task.name + "_fire", petri::TimeInterval(at, at), 0, petri::kControlTransitionCore, false);
  net.set_pre_arc(period_place, fire, 1);
  net.set_post_arc(fire, task.entry, 1);
  net.set_periodic_transition(task.name, fire);
}

// Two tasks used by the preemption tests:
//   A: started at 0, one [9,9] segment, deadline 10.
//   B: one-shot release at 2, one [9,9] segment, deadline 9 (absolute 11).
petri::PTPN make_release_order_net(SchedulePolicy policy) {
  petri::PTPN net;
  const HandTask a = add_hand_task(net, "A", {9}, 10, 0, /*start_at_zero=*/true);
  const HandTask b = add_hand_task(net, "B", {9}, 9, 2, /*start_at_zero=*/false);
  (void)a;
  add_one_shot_release(net, b, 2);
  net.set_schedule_policy(policy);
  return net;
}

// A: started at 0, segments [2,2] (C=4), deadline 10.
// B: one-shot release at 3, segment [2,2] (C=2), deadline 6.
petri::PTPN make_multi_segment_net(SchedulePolicy policy) {
  petri::PTPN net;
  const HandTask a = add_hand_task(net, "A", {2, 2}, 10, 0, /*start_at_zero=*/true);
  const HandTask b = add_hand_task(net, "B", {2}, 6, 3, /*start_at_zero=*/false);
  (void)a;
  add_one_shot_release(net, b, 3);
  net.set_schedule_policy(policy);
  return net;
}

// Two identical tasks on distinct real cores (plus identical completion
// consumers). Used only to exercise the symmetry guard: the verified task
// permutation group is non-trivial, so symmetry would actually be applied, and
// the guard fires before the rule ever indexes this net.
petri::PTPN make_symmetric_net() {
  petri::PTPN net;
  for (int core = 0; core < 2; ++core) {
    const std::string name = core == 0 ? "A" : "B";
    const HandTask task = add_hand_task(net, name, {1}, 5, 0, /*start_at_zero=*/true, core);
    const petri::PlaceId done = net.add_place(name + "_done", 1, true);
    const petri::TransitionId consume = net.add_transition(
        name + "_consume", petri::TimeInterval(0, 0), 0, petri::kControlTransitionCore, false);
    net.set_pre_arc(task.exit, consume, 1);
    net.set_post_arc(consume, done, 1);
  }
  return net;
}

std::string llf_at_zero_json(const std::string& policy) {
  return R"({
    "graph": {"name": "LlfAtZero"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": ")" +
         policy + R"(",
      "start": [
        {"task": "A", "tokens": 1},
        {"task": "B", "tokens": 1}
      ],
      "periodic": [
        {"task": "A", "period": 100},
        {"task": "B", "period": 50}
      ]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 10,
       "time": [[9, 9]], "locks": []},
      {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 5,
       "time": [[1, 1]], "locks": []}
    ],
    "edges": []
  })";
}

// ---------------------------------------------------------------------------
// 1. LLF differs from EDF/RM/DM at t=0 because urgency is not a deadline
// ---------------------------------------------------------------------------

TEST(LlfTest, LlfSelectsMaximumUrgencyWhileEdfAndStaticRulesPickTheDeadline) {
  // A: C=9, d=10 -> chi=-1. B: C=1, d=5 -> chi=-4.
  // LLF runs A first (B misses its deadline); EDF/RM/DM run B first.
  const std::vector<std::string> policies = {"llf", "edf", "rm", "dm"};
  for (const std::string& policy : policies) {
    const petri::PTPN net = lower(parse_tdg(llf_at_zero_json(policy)));
    const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
    const size_t ordinal_a = task_ordinal(tasks, "A");
    const size_t ordinal_b = task_ordinal(tasks, "B");
    ASSERT_NE(ordinal_a, SIZE_MAX) << policy;
    ASSERT_NE(ordinal_b, SIZE_MAX) << policy;
    const size_t a_get_core = chain_transition(net, "A", 1);
    const size_t b_get_core = chain_transition(net, "B", 1);
    ASSERT_NE(a_get_core, SIZE_MAX) << policy;
    ASSERT_NE(b_get_core, SIZE_MAX) << policy;

    StateClassReachabilityGraph graph(net);
    const std::vector<StateClass> initials = graph.compute_initial_classes();
    ASSERT_EQ(initials.size(), 1u) << policy;
    const StateClass& initial = initials.front();
    if (policy == "llf") {
      EXPECT_TRUE(state_class::contains(initial.priority_enabled, a_get_core)) << policy;
      EXPECT_FALSE(state_class::contains(initial.priority_enabled, b_get_core)) << policy;
      EXPECT_TRUE(initial.has_laxity_clock(ordinal_a));
      EXPECT_TRUE(initial.has_laxity_clock(ordinal_b));
      EXPECT_TRUE(chi_exact(initial, ordinal_a, 9 - 10));
      EXPECT_TRUE(chi_exact(initial, ordinal_b, 1 - 5));
      EXPECT_FALSE(initial.has_release_clock(ordinal_a));
    } else {
      EXPECT_TRUE(state_class::contains(initial.priority_enabled, b_get_core)) << policy;
      EXPECT_FALSE(state_class::contains(initial.priority_enabled, a_get_core)) << policy;
      EXPECT_FALSE(initial.has_laxity_clock(ordinal_a)) << policy;
    }

    const MetricsReport report = analyze(net);
    const TaskMetrics& a = task_metrics(report, "A");
    const TaskMetrics& b = task_metrics(report, "B");
    ASSERT_TRUE(a.observed) << policy;
    ASSERT_TRUE(b.observed) << policy;
    if (policy == "llf") {
      EXPECT_EQ(a.wcrt.value, 9) << policy;
      EXPECT_EQ(b.wcrt.value, 10) << policy;
      EXPECT_FALSE(a.deadline_missed) << policy;
      EXPECT_TRUE(b.deadline_missed) << policy;
    } else {
      EXPECT_EQ(a.wcrt.value, 10) << policy;
      EXPECT_EQ(b.wcrt.value, 1) << policy;
      EXPECT_FALSE(a.deadline_missed) << policy;
      EXPECT_FALSE(b.deadline_missed) << policy;
    }
  }
}

// ---------------------------------------------------------------------------
// 2. A release changes the laxity order: LLF preempts, EDF does not
// ---------------------------------------------------------------------------

TEST(LlfTest, ReleaseChangesLaxityOrderAndPreemptsTheRunningJob) {
  const petri::PTPN llf_net = make_release_order_net(SchedulePolicy::LLF);
  const petri::PTPN edf_net = make_release_order_net(SchedulePolicy::EDF);
  const std::vector<TaskChain> tasks = state_class::build_task_chains(llf_net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);
  const size_t a_exec = chain_transition(llf_net, "A", 3);
  const size_t b_exec = chain_transition(llf_net, "B", 3);
  ASSERT_NE(a_exec, SIZE_MAX);
  ASSERT_NE(b_exec, SIZE_MAX);

  const MetricsReport llf_report = analyze(llf_net);
  const MetricsReport edf_report = analyze(edf_net);
  EXPECT_EQ(task_metrics(llf_report, "A").wcrt.value, 18);
  EXPECT_EQ(task_metrics(llf_report, "B").wcrt.value, 9);
  EXPECT_EQ(task_metrics(edf_report, "A").wcrt.value, 9);
  EXPECT_EQ(task_metrics(edf_report, "B").wcrt.value, 16);
  EXPECT_TRUE(task_metrics(llf_report, "A").deadline_missed);
  EXPECT_FALSE(task_metrics(edf_report, "A").deadline_missed);

  StateClassReachabilityGraph llf_graph(llf_net);
  llf_graph.build(200000);
  ASSERT_FALSE(llf_graph.get_statistics().truncated);

  bool saw_b_running = false;
  bool saw_a_resumed = false;
  for (auto [it, end] = boost::vertices(llf_graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, llf_graph.get_graph(), *it);
    const bool a_in_flight = state_class::task_in_flight(tasks[ordinal_a], state.marking);
    const bool b_in_flight = state_class::task_in_flight(tasks[ordinal_b], state.marking);

    if (b_in_flight && state_class::contains(state.priority_enabled, b_exec) && a_in_flight) {
      saw_b_running = true;
      // A ran for 2 units before B's release; its execution clock is frozen.
      const int a_exec_index = state.exec_index(a_exec);
      ASSERT_GT(a_exec_index, 0);
      EXPECT_TRUE(state_class::contains(state.suspended, a_exec));
      EXPECT_EQ(-state.zone.get_constraint(0, static_cast<size_t>(a_exec_index)), 2);
      EXPECT_EQ(state.zone.get_constraint(static_cast<size_t>(a_exec_index), 0), 2);
      // At B's dispatch: chi_A = -1, chi_B = 0.
      EXPECT_TRUE(chi_exact(state, ordinal_a, -1));
      EXPECT_TRUE(chi_exact(state, ordinal_b, 0));

      const StateClass elapsed = llf_graph.time_elapse(state);
      const int b_exec_index = elapsed.exec_index(b_exec);
      ASSERT_GT(b_exec_index, 0);
      // Executing B freezes its chi; suspended A keeps h_A frozen at 2 while
      // its laxity advances. The elapsed zone is bounded by B's deterministic
      // segment end: chi_A rises from -1 to at most -1 + 9 = 8.
      EXPECT_EQ(elapsed.zone.get_constraint(static_cast<size_t>(b_exec_index), 0), 9);
      EXPECT_EQ(elapsed.zone.get_constraint(static_cast<size_t>(a_exec_index), 0), 2);
      EXPECT_EQ(laxity_bounds(elapsed, ordinal_a).lower, -1);
      EXPECT_EQ(laxity_bounds(elapsed, ordinal_a).upper, 8);
      EXPECT_TRUE(chi_exact(elapsed, ordinal_b, 0));
    }

    if (a_in_flight && !b_in_flight && state_class::contains(state.priority_enabled, a_exec)) {
      const int a_exec_index = state.exec_index(a_exec);
      ASSERT_GT(a_exec_index, 0);
      // A resumes with 7 units of budget left after executing 2. During B's
      // 9-unit run chi_A advanced from -1 to 8 and then stays frozen while A
      // executes.
      if (-state.zone.get_constraint(0, static_cast<size_t>(a_exec_index)) == 2 &&
          chi_exact(state, ordinal_a, 8)) {
        saw_a_resumed = true;
      }
    }
  }
  EXPECT_TRUE(saw_b_running);
  EXPECT_TRUE(saw_a_resumed);

  // EDF keeps A running at B's release: B waits at its entry.
  StateClassReachabilityGraph edf_graph(edf_net);
  edf_graph.build(200000);
  const std::vector<TaskChain> edf_tasks = state_class::build_task_chains(edf_net);
  const size_t edf_a = task_ordinal(edf_tasks, "A");
  const size_t edf_b = task_ordinal(edf_tasks, "B");
  const size_t edf_a_exec = chain_transition(edf_net, "A", 3);
  const size_t edf_b_get_core = chain_transition(edf_net, "B", 1);
  bool saw_edf_keeps_a = false;
  for (auto [it, end] = boost::vertices(edf_graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, edf_graph.get_graph(), *it);
    if (state_class::task_in_flight(edf_tasks[edf_a], state.marking) &&
        state_class::task_in_flight(edf_tasks[edf_b], state.marking) &&
        state_class::contains(state.priority_enabled, edf_a_exec) &&
        !state_class::contains(state.priority_enabled, edf_b_get_core)) {
      saw_edf_keeps_a = true;
      break;
    }
  }
  EXPECT_TRUE(saw_edf_keeps_a);
}

// ---------------------------------------------------------------------------
// 3. Equal urgency enumerates every tie
// ---------------------------------------------------------------------------

TEST(LlfTest, EqualUrgencyEnumeratesBothBranches) {
  petri::PTPN net;
  const HandTask a = add_hand_task(net, "A", {2}, 5, 0, /*start_at_zero=*/true);
  const HandTask b = add_hand_task(net, "B", {5}, 8, 0, /*start_at_zero=*/true);
  (void)a;
  (void)b;
  net.set_schedule_policy(SchedulePolicy::LLF);

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t b_get_core = chain_transition(net, "B", 1);
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);
  ASSERT_NE(a_get_core, SIZE_MAX);
  ASSERT_NE(b_get_core, SIZE_MAX);

  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 2u);

  int a_first = 0;
  int b_first = 0;
  for (const StateClass& state : initials) {
    EXPECT_TRUE(state.zone.is_consistent());
    EXPECT_FALSE(state.zone.is_empty());
    ASSERT_EQ(state.laxity_clock_of_task.size(), tasks.size());
    EXPECT_TRUE(state.has_laxity_clock(ordinal_a));
    EXPECT_TRUE(state.has_laxity_clock(ordinal_b));
    // chi = C - d: A = 2-5 = -3, B = 5-8 = -3.
    EXPECT_TRUE(chi_exact(state, ordinal_a, -3));
    EXPECT_TRUE(chi_exact(state, ordinal_b, -3));

    const bool picks_a = state_class::contains(state.priority_enabled, a_get_core);
    const bool picks_b = state_class::contains(state.priority_enabled, b_get_core);
    EXPECT_NE(picks_a, picks_b);
    a_first += picks_a ? 1 : 0;
    b_first += picks_b ? 1 : 0;
  }
  EXPECT_EQ(a_first, 1);
  EXPECT_EQ(b_first, 1);

  LlfFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(initials.front().struct_enabled, net, PolicyContext{net, &initials.front()});
  ASSERT_EQ(branches.size(), 2u);
  const auto find_branch = [&](size_t transition) -> const PolicyBranch* {
    for (const PolicyBranch& branch : branches) {
      if (state_class::contains(branch.active, transition)) {
        return &branch;
      }
    }
    return nullptr;
  };
  const PolicyBranch* a_branch = find_branch(a_get_core);
  const PolicyBranch* b_branch = find_branch(b_get_core);
  ASSERT_NE(a_branch, nullptr);
  ASSERT_NE(b_branch, nullptr);
  EXPECT_FALSE(state_class::contains(a_branch->active, b_get_core));
  EXPECT_FALSE(state_class::contains(b_branch->active, a_get_core));
  ASSERT_EQ(a_branch->constraints.size(), 1u);
  ASSERT_EQ(b_branch->constraints.size(), 1u);
  EXPECT_EQ(a_branch->constraints.front().bound, 0);
  EXPECT_EQ(a_branch->constraints.front().domain, ClockKind::Laxity);
  EXPECT_EQ(b_branch->constraints.front().bound, 0);
  EXPECT_EQ(b_branch->constraints.front().domain, ClockKind::Laxity);
  EXPECT_EQ(a_branch->constraints.front().left_task, ordinal_a);
  EXPECT_EQ(a_branch->constraints.front().right_task, ordinal_b);
  EXPECT_EQ(b_branch->constraints.front().left_task, ordinal_b);
  EXPECT_EQ(b_branch->constraints.front().right_task, ordinal_a);
}

// ---------------------------------------------------------------------------
// 4. Multi-segment preemption: h freezes, chi advances and is continuous
// ---------------------------------------------------------------------------

TEST(LlfTest, MultiSegmentPreemptionFreezesExecutionOnly) {
  const petri::PTPN net = make_multi_segment_net(SchedulePolicy::LLF);
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);
  const size_t a_exec1 = chain_transition(net, "A", 3);
  const size_t a_exec2 = chain_transition(net, "A", 5);
  const size_t b_exec = chain_transition(net, "B", 3);
  ASSERT_NE(a_exec1, SIZE_MAX);
  ASSERT_NE(a_exec2, SIZE_MAX);
  ASSERT_NE(b_exec, SIZE_MAX);

  const MetricsReport report = analyze(net);
  EXPECT_EQ(task_metrics(report, "A").wcrt.value, 6);
  EXPECT_EQ(task_metrics(report, "B").wcrt.value, 2);

  StateClassReachabilityGraph graph(net);
  graph.build(200000);
  ASSERT_FALSE(graph.get_statistics().truncated);

  // Chi is continuous across the deterministic first segment firing.
  bool saw_segment_continuity = false;
  bool saw_b_running = false;
  bool saw_resume = false;
  for (auto [it, end] = boost::edges(graph.get_graph()); it != end; ++it) {
    const auto source_vertex = boost::source(*it, graph.get_graph());
    const auto target_vertex = boost::target(*it, graph.get_graph());
    const state_class::FiringEdge& edge = boost::get(boost::edge_name, graph.get_graph(), *it);
    if (static_cast<size_t>(edge.transition_id) != a_exec1) {
      continue;
    }
    const StateClass& source = boost::get(boost::vertex_name, graph.get_graph(), source_vertex);
    const StateClass& target = boost::get(boost::vertex_name, graph.get_graph(), target_vertex);
    if (!state_class::task_in_flight(tasks[ordinal_a], target.marking)) {
      continue;
    }
    // First segment completed, second starts: chi_A stays exactly -6
    // (age 2 + remaining budget 2 - deadline 10), even though h_2 is fresh.
    EXPECT_TRUE(chi_exact(target, ordinal_a, -6));
    EXPECT_TRUE(chi_exact(source, ordinal_a, -6));
    EXPECT_TRUE(state_class::contains(target.priority_enabled, a_exec2));
    saw_segment_continuity = true;
  }
  EXPECT_TRUE(saw_segment_continuity);

  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, graph.get_graph(), *it);
    const bool a_in_flight = state_class::task_in_flight(tasks[ordinal_a], state.marking);
    const bool b_in_flight = state_class::task_in_flight(tasks[ordinal_b], state.marking);

    if (a_in_flight && b_in_flight && state_class::contains(state.priority_enabled, b_exec)) {
      saw_b_running = true;
      const int a_exec2_index = state.exec_index(a_exec2);
      ASSERT_GT(a_exec2_index, 0);
      // A executed 1 of its second segment's 2 units before B preempted it.
      EXPECT_EQ(-state.zone.get_constraint(0, static_cast<size_t>(a_exec2_index)), 1);
      EXPECT_TRUE(chi_exact(state, ordinal_a, -6));
      EXPECT_TRUE(chi_exact(state, ordinal_b, 2 - 6));

      const StateClass elapsed = graph.time_elapse(state);
      EXPECT_EQ(elapsed.zone.get_constraint(static_cast<size_t>(a_exec2_index), 0), 1);
      const int b_exec_index = elapsed.exec_index(b_exec);
      ASSERT_GT(b_exec_index, 0);
      EXPECT_EQ(elapsed.zone.get_constraint(static_cast<size_t>(b_exec_index), 0), 2);
      // A's laxity advances while B executes: -6 at B's dispatch, at most
      // -6 + 2 = -4 at B's deterministic segment end.
      EXPECT_EQ(laxity_bounds(elapsed, ordinal_a).lower, -6);
      EXPECT_EQ(laxity_bounds(elapsed, ordinal_a).upper, -4);
      EXPECT_TRUE(chi_exact(elapsed, ordinal_b, -4));
    }

    if (a_in_flight && !b_in_flight && state_class::contains(state.priority_enabled, a_exec2)) {
      const int a_exec2_index = state.exec_index(a_exec2);
      ASSERT_GT(a_exec2_index, 0);
      // A resumes with 1 unit left and chi advanced by B's 2-unit execution:
      // -6 + 2 = -4.
      if (-state.zone.get_constraint(0, static_cast<size_t>(a_exec2_index)) == 1 &&
          chi_exact(state, ordinal_a, -4)) {
        saw_resume = true;
      }
    }
  }
  EXPECT_TRUE(saw_b_running);
  EXPECT_TRUE(saw_resume);
}

// ---------------------------------------------------------------------------
// 5. Rate-law unit tests directly on time_elapse
// ---------------------------------------------------------------------------

TEST(LlfTest, TimeElapseRateLawForExecutingWaitingAndSegmentBoundaries) {
  petri::PTPN net;
  (void)add_hand_task(net, "A", {9}, 10, 0, /*start_at_zero=*/true);
  (void)add_hand_task(net, "B", {1}, 5, 0, /*start_at_zero=*/true);
  net.set_schedule_policy(SchedulePolicy::LLF);

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t a_exec = chain_transition(net, "A", 3);
  const size_t b_get_core = chain_transition(net, "B", 1);
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);

  StateClassReachabilityGraph graph(net);
  const std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 1u);
  ASSERT_TRUE(state_class::contains(initials.front().priority_enabled, a_get_core));
  ASSERT_TRUE(chi_exact(initials.front(), ordinal_a, -1));
  ASSERT_TRUE(chi_exact(initials.front(), ordinal_b, -4));

  // Dispatch A. A is now executing; B is waiting at its entry.
  const std::vector<StateClass> dispatched =
      graph.fire_all(graph.time_elapse(initials.front()), a_get_core);
  ASSERT_EQ(dispatched.size(), 1u);
  ASSERT_TRUE(state_class::contains(dispatched.front().priority_enabled, a_exec));
  ASSERT_FALSE(state_class::contains(dispatched.front().priority_enabled, b_get_core));

  const StateClass elapsed = graph.time_elapse(dispatched.front());
  // Executing A: chi frozen at its exact value.
  EXPECT_TRUE(chi_exact(elapsed, ordinal_a, -1));
  // Waiting at the entry (and filtered out of the active set): chi advances.
  // The elapsed zone is bounded by A's execution-segment deadline, so chi_B
  // rises from -4 to at most -4 + 9 = 5 while its lower bound is preserved.
  EXPECT_EQ(laxity_bounds(elapsed, ordinal_b).lower, -4);
  EXPECT_EQ(laxity_bounds(elapsed, ordinal_b).upper, 5);
  EXPECT_GT(laxity_bounds(elapsed, ordinal_b).upper,
            laxity_bounds(dispatched.front(), ordinal_b).upper);
  // The filtered get_core execution clock is frozen at zero.
  const int b_get_core_index = elapsed.exec_index(b_get_core);
  ASSERT_GT(b_get_core_index, 0);
  EXPECT_EQ(elapsed.zone.get_constraint(static_cast<size_t>(b_get_core_index), 0), 0);

  // After a deterministic segment firing, chi is unchanged. Use the
  // two-segment net so there is a boundary with a successor segment.
  const petri::PTPN multi_net = make_multi_segment_net(SchedulePolicy::LLF);
  const size_t seg1 = chain_transition(multi_net, "A", 3);
  const size_t seg2 = chain_transition(multi_net, "A", 5);
  ASSERT_NE(seg1, SIZE_MAX);
  ASSERT_NE(seg2, SIZE_MAX);
  StateClassReachabilityGraph multi_graph(multi_net);
  const std::vector<StateClass> multi_initials = multi_graph.compute_initial_classes();
  ASSERT_EQ(multi_initials.size(), 1u);
  const size_t multi_get_core = chain_transition(multi_net, "A", 1);
  const std::vector<StateClass> multi_dispatched =
      multi_graph.fire_all(multi_graph.time_elapse(multi_initials.front()), multi_get_core);
  ASSERT_EQ(multi_dispatched.size(), 1u);
  const StateClass before_boundary = multi_graph.time_elapse(multi_dispatched.front());
  const size_t multi_a = task_ordinal(state_class::build_task_chains(multi_net), "A");
  ASSERT_TRUE(chi_exact(before_boundary, multi_a, -6));
  const std::vector<StateClass> after_boundary = multi_graph.fire_all(before_boundary, seg1);
  ASSERT_EQ(after_boundary.size(), 1u);
  EXPECT_TRUE(state_class::contains(after_boundary.front().priority_enabled, seg2));
  EXPECT_TRUE(chi_exact(after_boundary.front(), multi_a, -6));
  EXPECT_EQ(laxity_bounds(after_boundary.front(), multi_a).lower,
            laxity_bounds(before_boundary, multi_a).lower);
  EXPECT_EQ(laxity_bounds(after_boundary.front(), multi_a).upper,
            laxity_bounds(before_boundary, multi_a).upper);

  // A preempted job's chi advances while its h stays frozen.
  const petri::PTPN preempt_net = make_release_order_net(SchedulePolicy::LLF);
  StateClassReachabilityGraph preempt_graph(preempt_net);
  preempt_graph.build(200000);
  const std::vector<TaskChain> preempt_tasks = state_class::build_task_chains(preempt_net);
  const size_t preempt_a = task_ordinal(preempt_tasks, "A");
  const size_t preempt_b = task_ordinal(preempt_tasks, "B");
  const size_t preempt_a_exec = chain_transition(preempt_net, "A", 3);
  const size_t preempt_b_exec = chain_transition(preempt_net, "B", 3);
  bool checked_preempted_rate = false;
  for (auto [it, end] = boost::vertices(preempt_graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, preempt_graph.get_graph(), *it);
    if (!state_class::contains(state.priority_enabled, preempt_b_exec) ||
        !state_class::task_in_flight(preempt_tasks[preempt_a], state.marking)) {
      continue;
    }
    const StateClass preempt_elapsed = preempt_graph.time_elapse(state);
    // A is preempted, so its chi advances from -1 to at most -1 + 9 = 8 while
    // B's executing chi stays frozen at 0.
    EXPECT_EQ(laxity_bounds(preempt_elapsed, preempt_a).lower, -1);
    EXPECT_EQ(laxity_bounds(preempt_elapsed, preempt_a).upper, 8);
    EXPECT_TRUE(chi_exact(preempt_elapsed, preempt_b, 0));
    const int a_index = preempt_elapsed.exec_index(preempt_a_exec);
    ASSERT_GT(a_index, 0);
    EXPECT_EQ(preempt_elapsed.zone.get_constraint(static_cast<size_t>(a_index), 0), 2);
    checked_preempted_rate = true;
    break;
  }
  EXPECT_TRUE(checked_preempted_rate);
}

// ---------------------------------------------------------------------------
// 6. Reset / carry / drop invariants of the laxity clock
// ---------------------------------------------------------------------------

TEST(LlfTest, LaxityLifecycleResetsCarriesAndDropsClock) {
  const petri::PTPN net = make_multi_segment_net(SchedulePolicy::LLF);
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  ASSERT_EQ(tasks.size(), 2u);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);

  StateClassReachabilityGraph graph(net);
  graph.build(200000);
  ASSERT_FALSE(graph.get_statistics().truncated);

  bool saw_fresh_b = false;
  bool saw_carry_segment = false;
  bool saw_completed_drop = false;

  for (auto [it, end] = boost::edges(graph.get_graph()); it != end; ++it) {
    const auto source_vertex = boost::source(*it, graph.get_graph());
    const auto target_vertex = boost::target(*it, graph.get_graph());
    const StateClass& source = boost::get(boost::vertex_name, graph.get_graph(), source_vertex);
    const StateClass& target = boost::get(boost::vertex_name, graph.get_graph(), target_vertex);

    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      const bool in_source = state_class::task_in_flight(tasks[ordinal], source.marking);
      const bool in_target = state_class::task_in_flight(tasks[ordinal], target.marking);
      if (!in_source && in_target) {
        // Fresh job: exactly chi = wcet - deadline.
        ASSERT_TRUE(target.has_laxity_clock(ordinal));
        const int expected = tasks[ordinal].wcet - tasks[ordinal].deadline;
        EXPECT_TRUE(chi_exact(target, ordinal, expected)) << tasks[ordinal].name;
        if (ordinal == ordinal_b) {
          saw_fresh_b = true;
        }
      } else if (in_source && !in_target) {
        // Completed job: its clock is gone and the exit place is marked.
        EXPECT_FALSE(target.has_laxity_clock(ordinal));
        EXPECT_GT(target.marking[tasks[ordinal].exit_place], 0);
        saw_completed_drop = true;
      } else if (in_source && in_target) {
        ASSERT_TRUE(target.has_laxity_clock(ordinal));
        const int source_index = source.laxity_index(ordinal);
        const int target_index = target.laxity_index(ordinal);
        ASSERT_GT(source_index, 0);
        ASSERT_GT(target_index, 0);
        // Firing one of the task's own chain transitions does not reset or
        // advance chi: execution froze it (deterministic segment).
        bool own_transition = false;
        for (size_t transition : tasks[ordinal].transitions) {
          if (static_cast<size_t>(
                  boost::get(boost::edge_name, graph.get_graph(), *it).transition_id) ==
              transition) {
            own_transition = true;
            break;
          }
        }
        if (own_transition) {
          EXPECT_EQ(source.zone.get_constraint(0, static_cast<size_t>(source_index)),
                    target.zone.get_constraint(0, static_cast<size_t>(target_index)));
          EXPECT_EQ(source.zone.get_constraint(static_cast<size_t>(source_index), 0),
                    target.zone.get_constraint(static_cast<size_t>(target_index), 0));
          if (ordinal == ordinal_a) {
            saw_carry_segment = true;
          }
        }
      }
    }
  }
  EXPECT_TRUE(saw_fresh_b);
  EXPECT_TRUE(saw_carry_segment);
  EXPECT_TRUE(saw_completed_drop);

  // A merged release into a still-pending entry creates no new job and does
  // not reset chi. (Not exercised by this net, which has no overlap; the
  // supported class rejects overlap instead.) Completed tasks never carry a
  // laxity clock in any stored state.
  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, graph.get_graph(), *it);
    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      EXPECT_EQ(state.has_laxity_clock(ordinal),
                state_class::task_in_flight(tasks[ordinal], state.marking))
          << tasks[ordinal].name;
    }
  }
}

// ---------------------------------------------------------------------------
// 7. Laxity layout, identity, and unsupported reductions
// ---------------------------------------------------------------------------

TEST(LlfTest, LaxityLayoutAndStateIdentity) {
  const petri::PTPN net = lower(parse_tdg(llf_at_zero_json("llf")));
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  ASSERT_EQ(tasks.size(), 2u);

  StateClassReachabilityGraph graph(net);
  graph.build(200000);
  ASSERT_FALSE(graph.get_statistics().truncated);
  ASSERT_GT(graph.get_statistics().total_states, 1u);

  bool saw_two_in_flight = false;
  bool saw_no_in_flight = false;
  bool have_identity_candidate = false;
  size_t identity_ordinal = SIZE_MAX;
  StateClass identity_state;

  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = boost::get(boost::vertex_name, graph.get_graph(), *it);
    ASSERT_EQ(state.zone.size(), state.clock_vars.size());
    ASSERT_FALSE(state.clock_vars.empty());
    EXPECT_EQ(state.clock_vars.front().kind, ClockKind::Zero);
    ASSERT_EQ(state.laxity_clock_of_task.size(), tasks.size());
    EXPECT_FALSE(state.has_release_clock(0));
    EXPECT_FALSE(state.has_release_clock(1));

    std::vector<size_t> in_flight_ordinals;
    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      const bool in_flight = state_class::task_in_flight(tasks[ordinal], state.marking);
      const int index = state.laxity_index(ordinal);
      if (in_flight) {
        in_flight_ordinals.push_back(ordinal);
        ASSERT_GT(index, 0) << tasks[ordinal].name;
        ASSERT_LT(static_cast<size_t>(index), state.clock_vars.size());
        EXPECT_EQ(state.clock_vars[static_cast<size_t>(index)].kind, ClockKind::Laxity);
        EXPECT_EQ(state.clock_vars[static_cast<size_t>(index)].task, ordinal);
        EXPECT_EQ(state.clock_vars[static_cast<size_t>(index)].transition,
                  state_class::NO_TRANSITION);
      } else {
        EXPECT_EQ(index, -1) << tasks[ordinal].name;
      }
    }
    saw_two_in_flight = saw_two_in_flight || in_flight_ordinals.size() == 2;
    saw_no_in_flight = saw_no_in_flight || in_flight_ordinals.empty();

    // Laxity clocks form a prefix immediately after x0, ascending ordinal.
    for (size_t i = 1; i <= in_flight_ordinals.size(); ++i) {
      ASSERT_LT(i, state.clock_vars.size());
      EXPECT_EQ(state.clock_vars[i].kind, ClockKind::Laxity);
      EXPECT_EQ(state.clock_vars[i].task, in_flight_ordinals[i - 1]);
    }
    for (size_t i = in_flight_ordinals.size() + 1; i < state.clock_vars.size(); ++i) {
      EXPECT_NE(state.clock_vars[i].kind, ClockKind::Laxity);
    }

    StateClass rebuilt = state;
    state_class::build_clock_layout(rebuilt, net, /*keep_suspension_clocks=*/false,
                                    /*include_release_clocks=*/false,
                                    /*include_laxity_clocks=*/true);
    ASSERT_EQ(rebuilt.clock_vars.size(), state.clock_vars.size());
    for (size_t i = 0; i < rebuilt.clock_vars.size(); ++i) {
      EXPECT_EQ(rebuilt.clock_vars[i].kind, state.clock_vars[i].kind) << i;
      EXPECT_EQ(rebuilt.clock_vars[i].transition, state.clock_vars[i].transition) << i;
      EXPECT_EQ(rebuilt.clock_vars[i].task, state.clock_vars[i].task) << i;
    }
    EXPECT_EQ(rebuilt.laxity_clock_of_task, state.laxity_clock_of_task);
    EXPECT_EQ(rebuilt.exec_clock_of_transition, state.exec_clock_of_transition);

    if (!have_identity_candidate && in_flight_ordinals.size() == 1) {
      const LaxityBounds bounds = laxity_bounds(state, in_flight_ordinals.front());
      if (bounds.finite) {
        identity_state = state;
        identity_ordinal = in_flight_ordinals.front();
        have_identity_candidate = true;
      }
    }
  }
  EXPECT_TRUE(saw_two_in_flight);
  EXPECT_TRUE(saw_no_in_flight);
  ASSERT_TRUE(have_identity_candidate);
  ASSERT_NE(identity_ordinal, SIZE_MAX);

  // Tracking release and laxity clocks at once is rejected, not silently
  // layered into one layout.
  StateClass both = identity_state;
  EXPECT_THROW(state_class::build_clock_layout(both, net, false, true, true),
               std::invalid_argument);

  // Different chi zones are different state identities. A completed state in
  // the graph above has a point zone, so build a genuinely ranged one: while A
  // executes a deterministic segment, the waiting job B advances its laxity
  // from -4 up to 5. Raising B's lower laxity bound by one intersects that
  // zone with a feasible constraint, yielding the same layout with a
  // different, still consistent zone.
  petri::PTPN range_net;
  (void)add_hand_task(range_net, "A", {9}, 10, 0, /*start_at_zero=*/true);
  (void)add_hand_task(range_net, "B", {1}, 5, 0, /*start_at_zero=*/true);
  range_net.set_schedule_policy(SchedulePolicy::LLF);
  const std::vector<TaskChain> range_tasks = state_class::build_task_chains(range_net);
  const size_t range_ordinal_b = task_ordinal(range_tasks, "B");
  ASSERT_NE(range_ordinal_b, SIZE_MAX);
  const size_t range_a_get_core = chain_transition(range_net, "A", 1);
  ASSERT_NE(range_a_get_core, SIZE_MAX);
  StateClassReachabilityGraph range_graph(range_net);
  const std::vector<StateClass> range_initials = range_graph.compute_initial_classes();
  ASSERT_EQ(range_initials.size(), 1u);
  const std::vector<StateClass> range_dispatched =
      range_graph.fire_all(range_graph.time_elapse(range_initials.front()), range_a_get_core);
  ASSERT_EQ(range_dispatched.size(), 1u);
  const StateClass ranged = range_graph.time_elapse(range_dispatched.front());
  const int chi_index = ranged.laxity_index(range_ordinal_b);
  ASSERT_GT(chi_index, 0);
  const size_t chi = static_cast<size_t>(chi_index);
  const int chi_lower = -ranged.zone.get_constraint(0, chi);
  const int upper = ranged.zone.get_constraint(chi, 0);
  ASSERT_NE(chi_lower, std::numeric_limits<int>::min());
  ASSERT_LT(chi_lower, upper);
  StateClass restricted = ranged;
  ASSERT_TRUE(restricted.zone.tighten(0, chi, -chi_lower - 1));
  EXPECT_TRUE(restricted.zone.is_consistent());
  EXPECT_FALSE(state_class::check_equality(ranged, restricted));
  EXPECT_NE(state_class::hash_state_class(ranged), state_class::hash_state_class(restricted));

  // A different job-clock map is a different layout even with the same zone.
  StateClass remapped = identity_state;
  remapped.laxity_clock_of_task[identity_ordinal] = -1;
  EXPECT_FALSE(state_class::check_equality(identity_state, remapped));
  EXPECT_NE(state_class::hash_state_class(identity_state), state_class::hash_state_class(remapped));

  // Extrapolation and symmetry are unsupported for job-clock states.
  StateClassReachabilityGraph llf_graph(net);
  EXPECT_THROW(llf_graph.set_extrapolation(true), std::invalid_argument);
  const petri::PTPN fixed_net = lower(parse_tdg(llf_at_zero_json("fixed")));
  StateClassReachabilityGraph fixed_graph(fixed_net);
  fixed_graph.set_extrapolation(true);
  EXPECT_THROW(fixed_graph.set_firing_rule(std::make_unique<LlfFiringRule>()),
               std::invalid_argument);

  // Symmetry: a single-core LLF net has no non-trivial task permutation, so
  // enabling it is harmless. On a net where symmetry is actually applied, the
  // lazy LLF rule is rejected before any laxity state is constructed.
  StateClassReachabilityGraph symmetry_llf(net);
  symmetry_llf.set_symmetry_reduction(true);
  EXPECT_FALSE(symmetry_llf.symmetry_applied());
  symmetry_llf.build(200000);
  EXPECT_FALSE(symmetry_llf.get_statistics().truncated);

  const petri::PTPN symmetric_net = make_symmetric_net();
  StateClassReachabilityGraph symmetric_graph(symmetric_net);
  symmetric_graph.set_symmetry_reduction(true);
  ASSERT_TRUE(symmetric_graph.symmetry_applied());
  symmetric_graph.set_firing_rule(std::make_unique<LlfFiringRule>());
  try {
    symmetric_graph.build(1000);
    FAIL() << "expected symmetry reduction to be rejected for laxity states";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("symmetry"), std::string::npos)
        << "unexpected message: " << error.what();
  }
}

// ---------------------------------------------------------------------------
// 8. Unsupported models are rejected with clear errors
// ---------------------------------------------------------------------------

TEST(LlfTest, RejectsUnsupportedLoweredModels) {
  // No real core at all.
  expect_rejected(R"({
    "graph": {"name": "LlfNoCpu"},
    "configuration": {"num_cpus": 0, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "num_cpus >= 1");

  // More than one core per CPU.
  expect_rejected(R"({
    "graph": {"name": "LlfCoresPerCpu"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 2, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "cores_per_cpu == 1");

  // Task on a core outside the declared range.
  expect_rejected(R"({
    "graph": {"name": "LlfCoreOne"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 1,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "outside [0, 1)");

  // Capacity above one (queued jobs are outside the supported class).
  expect_rejected(R"({
    "graph": {"name": "LlfCapacityTwo"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf",
                      "task_place_capacity": 2,
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "task_place_capacity == 1");

  // Locks.
  expect_rejected(R"({
    "graph": {"name": "LlfLocks"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf",
                      "shared_locks": ["m"],
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": ["m"]}],
    "edges": []
  })",
                  "does not include locks");

  // Fork/join.
  expect_rejected(R"({
    "graph": {"name": "LlfFork"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0,
       "deadline": 5, "time": [[1, 1]], "locks": []},
      {"id": "F", "type": "fork", "core": 0, "time": [[1, 1]]}
    ],
    "edges": []
  })",
                  "fork/join");

  // Dependency-driven release (task-to-task edge).
  expect_rejected(R"({
    "graph": {"name": "LlfDependency"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf",
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
  expect_rejected(R"({
    "graph": {"name": "LlfNotReleased"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf"},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "neither periodic");

  // Missing deadline and period.
  expect_rejected(R"({
    "graph": {"name": "LlfNoDeadline"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "positive absolute deadline or period");

  // More than one initial job for a task.
  expect_rejected(R"({
    "graph": {"name": "LlfTwoStarts"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 2}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "at most one initial job");

  // Interval (non-deterministic) execution times are unsupported: chi cannot
  // be carried exactly across a segment when work is not known.
  expect_rejected(R"({
    "graph": {"name": "LlfInterval"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "llf",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "deadline": 5, "time": [[1, 2]], "locks": []}],
    "edges": []
  })",
                  "deterministic execution segments");

  // An overlapping periodic release is rejected at the successor state, not
  // queued or merged silently.
  const char* overlapping = R"({
    "graph": {"name": "LlfOverlappingRelease"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "llf",
      "periodic": [{"task": "A", "period": 2}]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 4,
       "time": [[3, 3]], "locks": []}
    ],
    "edges": []
  })";
  const petri::PTPN overlap_net = lower(parse_tdg(overlapping));
  StateClassReachabilityGraph overlap_graph(overlap_net);
  try {
    overlap_graph.build(10000);
    FAIL() << "expected the overlapping release to be rejected";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("one job per task"), std::string::npos)
        << "unexpected message: " << error.what();
  }
}

TEST(LlfTest, RejectsHandBuiltNetsWithMissingOrInconsistentMetadata) {
  // No task metadata/chain.
  petri::PTPN hand_built;
  hand_built.add_transition("t", petri::TimeInterval(0, 0), 1, 0, false);
  try {
    LlfFiringRule rule(hand_built);
    FAIL() << "expected LLF to reject a net without task chains";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("no task chains"), std::string::npos)
        << "unexpected message: " << error.what();
  }

  // LLF has no scalar priority; a caller cannot silently fall back to one.
  LlfFiringRule unindexed;
  const PolicyContext context{hand_built, nullptr};
  EXPECT_THROW(unindexed.effective_priority(context, 0), std::logic_error);

  // TaskInfo::wcet must match the deterministic segment sum.
  petri::PTPN wrong_wcet;
  const HandTask a = add_hand_task(wrong_wcet, "A", {2}, 5, 0, /*start_at_zero=*/true);
  (void)a;
  petri::TaskInfo info = wrong_wcet.task_info().at("A");
  info.wcet = 1;
  wrong_wcet.set_task_info("A", info);
  wrong_wcet.set_schedule_policy(SchedulePolicy::LLF);
  try {
    LlfFiringRule rule(wrong_wcet);
    FAIL() << "expected LLF to reject inconsistent TaskInfo::wcet";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("TaskInfo::wcet"), std::string::npos)
        << "unexpected message: " << error.what();
  }

  // A hand-built interval segment is rejected by the rule itself.
  petri::PTPN interval;
  const petri::PlaceId entry = interval.add_place("A_entry", 1, true);
  const petri::TransitionId get_core =
      interval.add_transition("A_get_core", petri::TimeInterval(0, 0), 0, 0, false);
  const petri::PlaceId ready = interval.add_place("A_ready", 1, true);
  const petri::TransitionId interval_exec =
      interval.add_transition("A_exec", petri::TimeInterval(1, 2), 0, 0, true);
  const petri::PlaceId exit = interval.add_place("A_exit", 1, true);
  interval.set_pre_arc(entry, get_core);
  interval.set_post_arc(get_core, ready);
  interval.set_pre_arc(ready, interval_exec);
  interval.set_post_arc(interval_exec, exit);
  interval.set_initial_marking(entry, 1);
  interval.set_task_chain(
      "A", {petri::NodeRef::of(entry), petri::NodeRef::of(get_core), petri::NodeRef::of(ready),
            petri::NodeRef::of(interval_exec), petri::NodeRef::of(exit)});
  petri::TaskInfo interval_info;
  interval_info.core = 0;
  interval_info.wcet = 2;
  interval_info.deadline = 5;
  interval.set_task_info("A", interval_info);
  interval.set_schedule_policy(SchedulePolicy::LLF);
  try {
    LlfFiringRule rule(interval);
    FAIL() << "expected LLF to reject an interval execution segment";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("deterministic execution segments"), std::string::npos)
        << "unexpected message: " << error.what();
  }

  // A hand-built task bound to any real core is accepted: LLF indexes tasks by
  // their declared core and the scheduling rule compares urgency only within a
  // core.
  petri::PTPN off_core;
  (void)add_hand_task(off_core, "A", {1}, 5, 0, /*start_at_zero=*/true, /*core=*/1);
  off_core.set_schedule_policy(SchedulePolicy::LLF);
  LlfFiringRule off_core_rule(off_core);
  ASSERT_EQ(off_core_rule.tasks().size(), 1U);
  EXPECT_EQ(off_core_rule.tasks()[0].core, 1);
}

// ---------------------------------------------------------------------------
// 10. Symbolic invariant: an active job has maximum urgency in its zone
// ---------------------------------------------------------------------------

TEST(LlfTest, EveryStoredActiveStateEnforcesMaximumUrgency) {
  const auto check_graph = [](const petri::PTPN& net) {
    const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
    StateClassReachabilityGraph graph(net);
    graph.build(200000);
    ASSERT_FALSE(graph.get_statistics().truncated);
    size_t checked = 0;
    for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
      const StateClass& state = boost::get(boost::vertex_name, graph.get_graph(), *it);
      std::vector<size_t> in_flight;
      size_t active = SIZE_MAX;
      size_t active_count = 0;
      for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
        if (!state_class::task_in_flight(tasks[ordinal], state.marking)) {
          continue;
        }
        in_flight.push_back(ordinal);
        if (task_active(state, tasks[ordinal])) {
          active = ordinal;
          ++active_count;
        }
      }
      EXPECT_LE(active_count, 1u) << "state " << state.id;
      if (active_count != 1) {
        continue;
      }
      const int active_clock = state.laxity_index(active);
      ASSERT_GT(active_clock, 0);
      for (size_t other : in_flight) {
        if (other == active) {
          continue;
        }
        const int other_clock = state.laxity_index(other);
        ASSERT_GT(other_clock, 0);
        const int bound = state.zone.get_constraint(static_cast<size_t>(other_clock),
                                                    static_cast<size_t>(active_clock));
        EXPECT_NE(bound, INF_TIME) << "state " << state.id << " task " << tasks[other].name
                                   << " vs " << tasks[active].name;
        EXPECT_LE(bound, 0) << "state " << state.id << " task " << tasks[other].name
                            << " is more urgent than active " << tasks[active].name;
        ++checked;
      }
    }
    EXPECT_GT(checked, 0u);
  };

  check_graph(make_release_order_net(SchedulePolicy::LLF));
  check_graph(make_multi_segment_net(SchedulePolicy::LLF));
  check_graph(lower(parse_tdg(llf_at_zero_json("llf"))));
}

}  // namespace
