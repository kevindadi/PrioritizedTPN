#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "analysis/dbm/dbm.h"
#include "analysis/metrics/metrics.h"
#include "analysis/reachability/canonicalization.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"
#include "analysis/reachability/scheduling_policy.h"
#include "analysis/reachability/state_class.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "lower/tdg2pn/tdg2pn_common.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "parse/json.h"

namespace {

using state_class::BranchConstraint;
using state_class::ClockKind;
using state_class::FifoFiringRule;
using state_class::FiringEdge;
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

const StateClass& state_of(const state_class::SCGraph& graph, state_class::SCVertex vertex) {
  return boost::get(boost::vertex_name, graph, vertex);
}

bool any_task_running(const std::vector<TaskChain>& tasks, const std::vector<int>& marking,
                      size_t* running_ordinal = nullptr) {
  size_t count = 0;
  for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
    const TaskChain& task = tasks[ordinal];
    if (state_class::task_in_flight(task, marking) && marking[task.entry_place] <= 0) {
      ++count;
      if (running_ordinal != nullptr) {
        *running_ordinal = ordinal;
      }
    }
  }
  return count > 0;
}

std::pair<int, int> release_bounds(const StateClass& state, int release_index) {
  // lower = -upper(x0 - r), upper = upper(r - x0).
  return {-state.zone.get_constraint(0, static_cast<size_t>(release_index)),
          state.zone.get_constraint(static_cast<size_t>(release_index), 0)};
}

// FIFO vs a static priority policy:
//   H: periodic period 5, execution [1,1], priority 3
//   L: one-shot start at 0, execution [8,8], priority 1
// FIFO is non-preemptive: L runs 0..8, H waits 5..8 and finishes at 9.
// Fixed priority preempts L at 5: H finishes at 6 and L at 9.
std::string fifo_vs_fixed_json(const std::string& policy) {
  return R"({
    "graph": {"name": "FifoVsFixed"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": ")" +
         policy + R"(",
      "start": [{"task": "L", "tokens": 1}],
      "periodic": [{"task": "H", "period": 5}]
    },
    "nodes": [
      {"id": "H", "type": "task", "priority": 3, "core": 0,
       "time": [[1, 1]], "locks": []},
      {"id": "L", "type": "task", "priority": 1, "core": 0,
       "time": [[8, 8]], "locks": []}
    ],
    "edges": []
  })";
}

const char* kFifoOneShotJson = R"({
  "graph": {"name": "FifoOneShot"},
  "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fifo"},
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// Two jobs released together at time 0. Both dispatch orders are valid.
const char* kEqualArrivalJson = R"({
  "graph": {"name": "FifoEqualArrival"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "fifo",
    "start": [
      {"task": "A", "tokens": 1},
      {"task": "B", "tokens": 1}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []}
  ],
  "edges": []
})";

// L runs 0..9. A is released at 7 and B at 8, so when the core becomes free
// only A (release clock 2, B release clock 1) can be earliest.
const char* kFixedArrivalOrderJson = R"({
  "graph": {"name": "FifoFixedArrivalOrder"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "fifo",
    "start": [{"task": "L", "tokens": 1}],
    "periodic": [
      {"task": "A", "period": 7},
      {"task": "B", "period": 8}
    ]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []},
    {"id": "B", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []},
    {"id": "L", "type": "task", "priority": 0, "core": 0,
     "time": [[9, 9]], "locks": []}
  ],
  "edges": []
})";

// A periodic task whose first job waits behind a long one-shot job. Its
// releases at 5 and 10 merge while the job is pending; it dispatches at 13.
const char* kPendingMergeJson = R"({
  "graph": {"name": "FifoPendingMerge"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "fifo",
    "start": [{"task": "L", "tokens": 1}],
    "periodic": [{"task": "A", "period": 5}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1]], "locks": []},
    {"id": "L", "type": "task", "priority": 0, "core": 0,
     "time": [[13, 13]], "locks": []}
  ],
  "edges": []
})";

// A response time (3) exceeding the period (2): the second release necessarily
// fires while the first job is mid-chain. The one-job class rejects it.
const char* kOverlappingReleaseJson = R"({
  "graph": {"name": "FifoOverlappingRelease"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "fifo",
    "periodic": [{"task": "A", "period": 2}]
  },
  "nodes": [
    {"id": "A", "type": "task", "priority": 0, "core": 0,
     "time": [[3, 3]], "locks": []}
  ],
  "edges": []
})";

// ---------------------------------------------------------------------------
// 1. FIFO is non-preemptive, fixed priority preempts
// ---------------------------------------------------------------------------

TEST(FifoTest, FifoDoesNotPreemptAHighPriorityArrival) {
  const petri::PTPN fifo_net = lower(parse_tdg(fifo_vs_fixed_json("fifo")));
  const petri::PTPN fixed_net = lower(parse_tdg(fifo_vs_fixed_json("fixed")));
  ASSERT_EQ(fifo_net.schedule_policy(), SchedulePolicy::FIFO);
  ASSERT_EQ(fixed_net.schedule_policy(), SchedulePolicy::FIXED);

  const MetricsReport fifo = analyze(fifo_net);
  const MetricsReport fixed = analyze(fixed_net);

  const TaskMetrics& fifo_l = task_metrics(fifo, "L");
  const TaskMetrics& fifo_h = task_metrics(fifo, "H");
  const TaskMetrics& fixed_l = task_metrics(fixed, "L");
  const TaskMetrics& fixed_h = task_metrics(fixed, "H");

  ASSERT_TRUE(fifo_l.observed);
  ASSERT_TRUE(fifo_h.observed);
  ASSERT_TRUE(fixed_l.observed);
  ASSERT_TRUE(fixed_h.observed);

  // FIFO: L finishes its 8 units before H (released at 5) runs 8..9.
  EXPECT_EQ(fifo_l.wcrt.value, 8);
  EXPECT_EQ(fifo_h.wcrt.value, 4);
  EXPECT_EQ(fifo_l.max_preemptions, 0);
  EXPECT_EQ(fifo_h.max_preemptions, 0);

  // Fixed priority: H preempts at 5 (response 1) and L resumes to finish at 9.
  EXPECT_EQ(fixed_l.wcrt.value, 9);
  EXPECT_EQ(fixed_h.wcrt.value, 1);
  EXPECT_EQ(fixed_l.max_preemptions, 1);

  EXPECT_LT(fifo_l.wcrt.value, fixed_l.wcrt.value);
  EXPECT_GT(fifo_h.wcrt.value, fixed_h.wcrt.value);
}

// ---------------------------------------------------------------------------
// 2. Equal release values enumerate both dispatch orders
// ---------------------------------------------------------------------------

TEST(FifoTest, EqualArrivalTiesEnumerateBothDispatchOrders) {
  const petri::PTPN net = lower(parse_tdg(kEqualArrivalJson));
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
    EXPECT_TRUE(state.has_release_clock(ordinal_a));
    EXPECT_TRUE(state.has_release_clock(ordinal_b));
    const bool picks_a = state_class::contains(state.priority_enabled, a_get_core);
    const bool picks_b = state_class::contains(state.priority_enabled, b_get_core);
    EXPECT_NE(picks_a, picks_b);
    a_first += picks_a ? 1 : 0;
    b_first += picks_b ? 1 : 0;
  }
  EXPECT_EQ(a_first, 1);
  EXPECT_EQ(b_first, 1);

  // Both dispatch orders are actually explored: each task can run while the
  // other waits at its entry, and the wait is reflected in a positive release
  // clock when the second job is finally dispatched.
  const std::vector<TaskChain>& chain = tasks;
  const size_t a_entry = chain[ordinal_a].entry_place;
  const size_t b_entry = chain[ordinal_b].entry_place;
  const size_t a_exit = chain[ordinal_a].exit_place;
  const size_t b_exit = chain[ordinal_b].exit_place;
  const int r_a_index = initials.front().release_index(ordinal_a);
  const int r_b_index = initials.front().release_index(ordinal_b);
  ASSERT_GT(r_a_index, 0);
  ASSERT_GT(r_b_index, 0);

  StateClassReachabilityGraph full_graph(net);
  full_graph.build(50000);
  ASSERT_FALSE(full_graph.get_statistics().truncated);

  bool saw_a_first = false;
  bool saw_b_first = false;
  bool saw_a_waited = false;
  bool saw_b_waited = false;
  for (auto [it, end] = boost::edges(full_graph.get_graph()); it != end; ++it) {
    const FiringEdge& edge = boost::get(boost::edge_name, full_graph.get_graph(), *it);
    const StateClass& source =
        state_of(full_graph.get_graph(), boost::source(*it, full_graph.get_graph()));
    const StateClass& target =
        state_of(full_graph.get_graph(), boost::target(*it, full_graph.get_graph()));

    if (edge.transition_id == static_cast<int>(a_get_core) && source.marking[a_entry] > 0 &&
        source.marking[b_entry] > 0 && target.marking[a_entry] == 0 &&
        target.marking[b_entry] > 0) {
      saw_a_first = true;
    }
    if (edge.transition_id == static_cast<int>(b_get_core) && source.marking[b_entry] > 0 &&
        source.marking[a_entry] > 0 && target.marking[b_entry] == 0 &&
        target.marking[a_entry] > 0) {
      saw_b_first = true;
    }

    // The second job dispatches after the first completed, with a release
    // clock equal to the first job's response time (1).
    if (edge.transition_id == static_cast<int>(a_get_core) && source.marking[a_entry] > 0 &&
        source.marking[b_exit] > 0) {
      const int r = source.release_index(ordinal_a);
      ASSERT_GT(r, 0);
      EXPECT_EQ(release_bounds(source, r), std::make_pair(1, 1));
      saw_a_waited = true;
    }
    if (edge.transition_id == static_cast<int>(b_get_core) && source.marking[b_entry] > 0 &&
        source.marking[a_exit] > 0) {
      const int r = source.release_index(ordinal_b);
      ASSERT_GT(r, 0);
      EXPECT_EQ(release_bounds(source, r), std::make_pair(1, 1));
      saw_b_waited = true;
    }
  }
  EXPECT_TRUE(saw_a_first);
  EXPECT_TRUE(saw_b_first);
  EXPECT_TRUE(saw_a_waited);
  EXPECT_TRUE(saw_b_waited);
}

// ---------------------------------------------------------------------------
// 3. Symbolic arrival ambiguity: both constraints are feasible in one zone
// ---------------------------------------------------------------------------

TEST(FifoTest, SymbolicArrivalOrderEmitsBothFeasibleBranches) {
  const petri::PTPN net = lower(parse_tdg(kEqualArrivalJson));
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

  StateClass relaxed = initials.front();
  const int r_a = relaxed.release_index(ordinal_a);
  const int r_b = relaxed.release_index(ordinal_b);
  ASSERT_GT(r_a, 0);
  ASSERT_GT(r_b, 0);

  // r_A ranges over [0,4], r_B stays 0, so either task may have arrived first.
  const size_t r_a_index = static_cast<size_t>(r_a);
  relaxed.zone.set_constraint(r_a_index, 0, 4);
  for (size_t j = 1; j < relaxed.zone.size(); ++j) {
    if (j != r_a_index) {
      relaxed.zone.set_constraint(r_a_index, j, 4);
    }
  }
  relaxed.zone.minimize();
  ASSERT_TRUE(relaxed.zone.is_consistent());

  FifoFiringRule rule(net);
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

  const PolicyBranch* a_branch = find_branch(a_get_core);
  const PolicyBranch* b_branch = find_branch(b_get_core);
  ASSERT_NE(a_branch, nullptr);
  ASSERT_NE(b_branch, nullptr);
  EXPECT_FALSE(state_class::contains(a_branch->active, b_get_core));
  EXPECT_FALSE(state_class::contains(b_branch->active, a_get_core));

  // A is earliest iff r_B - r_A <= 0; B is earliest iff r_A - r_B <= 0.
  ASSERT_EQ(a_branch->constraints.size(), 1u);
  EXPECT_EQ(a_branch->constraints.front().left_task, ordinal_a);
  EXPECT_EQ(a_branch->constraints.front().right_task, ordinal_b);
  EXPECT_EQ(a_branch->constraints.front().bound, 0);
  ASSERT_EQ(b_branch->constraints.size(), 1u);
  EXPECT_EQ(b_branch->constraints.front().left_task, ordinal_b);
  EXPECT_EQ(b_branch->constraints.front().right_task, ordinal_a);
  EXPECT_EQ(b_branch->constraints.front().bound, 0);

  // Intersecting each constraint yields exactly the arrival order it selects.
  state_class::DBM a_zone = relaxed.zone;
  EXPECT_TRUE(a_zone.tighten(static_cast<size_t>(r_b), static_cast<size_t>(r_a), 0));
  EXPECT_EQ(a_zone.get_constraint(static_cast<size_t>(r_b), static_cast<size_t>(r_a)), 0);

  state_class::DBM b_zone = relaxed.zone;
  EXPECT_TRUE(b_zone.tighten(static_cast<size_t>(r_a), static_cast<size_t>(r_b), 0));
  EXPECT_EQ(b_zone.get_constraint(static_cast<size_t>(r_a), static_cast<size_t>(r_b)), 0);
  // r_B is pinned to zero, so B-earliest forces r_A to zero as well.
  EXPECT_EQ(b_zone.get_constraint(static_cast<size_t>(r_a), 0), 0);
}

// ---------------------------------------------------------------------------
// 4. Timed releases fix the arrival order: exactly one dispatch branch
// ---------------------------------------------------------------------------

TEST(FifoTest, FixedArrivalOrderEmitsSingleDispatchBranch) {
  const petri::PTPN net = lower(parse_tdg(kFixedArrivalOrderJson));
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  const size_t ordinal_l = task_ordinal(tasks, "L");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);
  ASSERT_NE(ordinal_l, SIZE_MAX);
  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t b_get_core = chain_transition(net, "B", 1);
  ASSERT_NE(a_get_core, SIZE_MAX);
  ASSERT_NE(b_get_core, SIZE_MAX);

  StateClassReachabilityGraph graph(net);
  graph.build(50000);
  ASSERT_FALSE(graph.get_statistics().truncated);

  const StateClass* found = nullptr;
  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = state_of(graph.get_graph(), *it);
    if (state.marking[tasks[ordinal_l].exit_place] > 0 &&
        state.marking[tasks[ordinal_a].entry_place] > 0 &&
        state.marking[tasks[ordinal_b].entry_place] > 0) {
      found = &state;
      break;
    }
  }
  ASSERT_NE(found, nullptr) << "no state with L completed and A/B pending";

  const int r_a = found->release_index(ordinal_a);
  const int r_b = found->release_index(ordinal_b);
  ASSERT_GT(r_a, 0);
  ASSERT_GT(r_b, 0);
  // A arrived at t=7 and B at t=8 relative to L's completion at t=9.
  EXPECT_EQ(release_bounds(*found, r_a), std::make_pair(2, 2));
  EXPECT_EQ(release_bounds(*found, r_b), std::make_pair(1, 1));
  EXPECT_EQ(found->zone.get_constraint(static_cast<size_t>(r_b), static_cast<size_t>(r_a)), -1);

  FifoFiringRule rule(net);
  const std::vector<PolicyBranch> branches =
      rule.branches(found->struct_enabled, net, PolicyContext{net, found});
  ASSERT_EQ(branches.size(), 1u);
  EXPECT_TRUE(state_class::contains(branches.front().active, a_get_core));
  EXPECT_FALSE(state_class::contains(branches.front().active, b_get_core));
  ASSERT_EQ(branches.front().constraints.size(), 1u);
  EXPECT_EQ(branches.front().constraints.front().left_task, ordinal_a);
  EXPECT_EQ(branches.front().constraints.front().right_task, ordinal_b);
  EXPECT_EQ(branches.front().constraints.front().bound, 0);
}

// ---------------------------------------------------------------------------
// 5. Non-preemption is enforced by the marking and the structural CPU place
// ---------------------------------------------------------------------------

TEST(FifoTest, RunningJobKeepsTheCoreUntilExit) {
  const petri::PTPN net = lower(parse_tdg(fifo_vs_fixed_json("fifo")));
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_h = task_ordinal(tasks, "H");
  const size_t ordinal_l = task_ordinal(tasks, "L");
  ASSERT_NE(ordinal_h, SIZE_MAX);
  ASSERT_NE(ordinal_l, SIZE_MAX);

  // Non-resume lowering: one structural core place, capacity 1, and no
  // suspendable execution segments.
  ASSERT_EQ(net.cpu_places().size(), 1u);
  const petri::Place& core = net.get_place(net.cpu_places().front());
  EXPECT_EQ(core.name, "core0");
  EXPECT_EQ(core.capacity, 1);
  EXPECT_EQ(net.get_marking()[net.cpu_places().front().index()], 1);
  EXPECT_FALSE(converter::detail::is_resume_policy(SchedulePolicy::FIFO));
  for (size_t t = 0; t < net.num_transitions(); ++t) {
    EXPECT_FALSE(net.get_transition(petri::TransitionId{t}).suspendable)
        << net.get_transition(petri::TransitionId{t}).name;
  }

  // The net-aware factory installs the dynamic FIFO rule, not a static
  // non-preemptive fallback.
  std::unique_ptr<state_class::FiringRule> fifo_rule =
      state_class::make_firing_rule(SchedulePolicy::FIFO, net);
  ASSERT_NE(fifo_rule, nullptr);
  EXPECT_NE(dynamic_cast<FifoFiringRule*>(fifo_rule.get()), nullptr);
  EXPECT_TRUE(fifo_rule->uses_release_clocks());
  EXPECT_EQ(fifo_rule->preemption_mode(), state_class::PreemptionMode::NON_PREEMPTIVE);

  const size_t h_get_core = chain_transition(net, "H", 1);
  ASSERT_NE(h_get_core, SIZE_MAX);

  StateClassReachabilityGraph graph(net);
  graph.build(50000);
  ASSERT_FALSE(graph.get_statistics().truncated);

  bool saw_l_running_with_h_pending = false;
  size_t max_running = 0;
  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = state_of(graph.get_graph(), *it);
    EXPECT_TRUE(state.suspended.empty());

    size_t running = 0;
    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      if (state_class::task_in_flight(tasks[ordinal], state.marking) &&
          state.marking[tasks[ordinal].entry_place] <= 0) {
        ++running;
      }
    }
    max_running = std::max(max_running, running);

    const bool l_running = state_class::task_in_flight(tasks[ordinal_l], state.marking) &&
                           state.marking[tasks[ordinal_l].entry_place] <= 0;
    const bool h_pending = state.marking[tasks[ordinal_h].entry_place] > 0;
    if (l_running && h_pending) {
      saw_l_running_with_h_pending = true;
      // The high-priority arrival is not admitted while L runs.
      EXPECT_FALSE(state_class::contains(state.priority_enabled, h_get_core));
    }
  }
  EXPECT_LE(max_running, 1u);
  EXPECT_TRUE(saw_l_running_with_h_pending);
}

// ---------------------------------------------------------------------------
// 6. Release-clock lifecycle across pending, dispatch, merge and completion
// ---------------------------------------------------------------------------

TEST(FifoTest, ReleaseClockLifecycleAcrossDispatchMergeAndCompletion) {
  const petri::PTPN net = lower(parse_tdg(kPendingMergeJson));
  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_l = task_ordinal(tasks, "L");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_l, SIZE_MAX);
  const size_t a_fire = find_transition(net, "A_fire");
  const size_t a_get_core = chain_transition(net, "A", 1);
  const size_t l_last_exec = chain_transition(net, "L", tasks[ordinal_l].chain.size() - 2);
  ASSERT_NE(a_fire, SIZE_MAX);
  ASSERT_NE(a_get_core, SIZE_MAX);
  ASSERT_NE(l_last_exec, SIZE_MAX);

  StateClassReachabilityGraph graph(net);
  graph.build(50000);
  ASSERT_FALSE(graph.get_statistics().truncated);

  // Initial job clock is zero and the layout puts release clocks first.
  StateClass initial = graph.compute_initial_class();
  const int initial_l = initial.release_index(ordinal_l);
  ASSERT_GT(initial_l, 0);
  EXPECT_EQ(release_bounds(initial, initial_l), std::make_pair(0, 0));
  EXPECT_EQ(initial.release_index(ordinal_a), -1);
  EXPECT_EQ(initial.clock_vars[1].kind, ClockKind::Release);
  EXPECT_EQ(initial.clock_vars[1].task, ordinal_l);

  bool saw_merge = false;
  bool saw_dispatch_after_wait = false;
  bool saw_completion_drop = false;

  for (auto [it, end] = boost::edges(graph.get_graph()); it != end; ++it) {
    const FiringEdge& edge = boost::get(boost::edge_name, graph.get_graph(), *it);
    const StateClass& source = state_of(graph.get_graph(), boost::source(*it, graph.get_graph()));
    const StateClass& target = state_of(graph.get_graph(), boost::target(*it, graph.get_graph()));

    if (edge.transition_id == static_cast<int>(a_fire)) {
      const bool source_pending = source.marking[tasks[ordinal_a].entry_place] > 0;
      const bool target_pending = target.marking[tasks[ordinal_a].entry_place] > 0;
      if (source_pending && target_pending) {
        // A release merged into the pending job at t=10 (its release was at 5).
        // The release clock must survive with value 5, not reset to zero.
        const int r_a = target.release_index(ordinal_a);
        ASSERT_GT(r_a, 0);
        EXPECT_GT(release_bounds(target, r_a).second, 0);
        EXPECT_EQ(release_bounds(target, r_a), std::make_pair(5, 5));
        EXPECT_EQ(source.release_index(ordinal_a), r_a);
        saw_merge = true;
      }
    }

    if (edge.transition_id == static_cast<int>(a_get_core)) {
      const bool source_pending = source.marking[tasks[ordinal_a].entry_place] > 0;
      const bool target_running = state_class::task_in_flight(tasks[ordinal_a], target.marking) &&
                                  target.marking[tasks[ordinal_a].entry_place] <= 0;
      if (source_pending && target_running) {
        const int r_source = source.release_index(ordinal_a);
        const int r_target = target.release_index(ordinal_a);
        ASSERT_GT(r_source, 0);
        ASSERT_GT(r_target, 0);
        // Pending -> running preserves the release clock column and value.
        EXPECT_EQ(r_source, r_target);
        EXPECT_EQ(release_bounds(target, r_target), release_bounds(source, r_source));
        if (release_bounds(target, r_target) == std::make_pair(8, 8)) {
          saw_dispatch_after_wait = true;
        }
      }
    }

    if (edge.transition_id == static_cast<int>(l_last_exec)) {
      // Completion drops the release clock (the exit token is not in flight).
      EXPECT_TRUE(target.marking[tasks[ordinal_l].exit_place] > 0);
      EXPECT_FALSE(state_class::task_in_flight(tasks[ordinal_l], target.marking));
      EXPECT_EQ(target.release_index(ordinal_l), -1);
      saw_completion_drop = true;
    }
  }

  EXPECT_TRUE(saw_merge);
  EXPECT_TRUE(saw_dispatch_after_wait);
  EXPECT_TRUE(saw_completion_drop);

  // Release clocks exist exactly for in-flight jobs and form the canonical
  // prefix immediately after x0, in task-ordinal order.
  for (auto [it, end] = boost::vertices(graph.get_graph()); it != end; ++it) {
    const StateClass& state = state_of(graph.get_graph(), *it);
    ASSERT_EQ(state.zone.size(), state.clock_vars.size());
    ASSERT_EQ(state.release_clock_of_task.size(), tasks.size());
    std::vector<size_t> in_flight;
    for (size_t ordinal = 0; ordinal < tasks.size(); ++ordinal) {
      const bool flying = state_class::task_in_flight(tasks[ordinal], state.marking);
      const int release_index = state.release_index(ordinal);
      if (flying) {
        in_flight.push_back(ordinal);
        ASSERT_GT(release_index, 0);
        EXPECT_EQ(state.clock_vars[static_cast<size_t>(release_index)].kind, ClockKind::Release);
        EXPECT_EQ(state.clock_vars[static_cast<size_t>(release_index)].task, ordinal);
      } else {
        EXPECT_EQ(release_index, -1);
      }
      EXPECT_LE(state_class::task_chain_token_count(tasks[ordinal], state.marking), 1u);
    }
    for (size_t i = 1; i <= in_flight.size(); ++i) {
      ASSERT_LT(i, state.clock_vars.size());
      EXPECT_EQ(state.clock_vars[i].kind, ClockKind::Release);
      EXPECT_EQ(state.clock_vars[i].task, in_flight[i - 1]);
    }
    for (size_t i = in_flight.size() + 1; i < state.clock_vars.size(); ++i) {
      EXPECT_NE(state.clock_vars[i].kind, ClockKind::Release);
    }
  }
}

// ---------------------------------------------------------------------------
// 7. Arrival-order differences are part of the state identity
// ---------------------------------------------------------------------------

TEST(FifoTest, ArrivalOrderZoneDifferencesDoNotMerge) {
  const petri::PTPN net = lower(parse_tdg(kEqualArrivalJson));
  StateClassReachabilityGraph graph(net);
  std::vector<StateClass> initials = graph.compute_initial_classes();
  ASSERT_EQ(initials.size(), 2u);

  const std::vector<TaskChain> tasks = state_class::build_task_chains(net);
  const size_t ordinal_a = task_ordinal(tasks, "A");
  const size_t ordinal_b = task_ordinal(tasks, "B");
  ASSERT_NE(ordinal_a, SIZE_MAX);
  ASSERT_NE(ordinal_b, SIZE_MAX);

  StateClass relaxed = initials.front();
  const int r_a = relaxed.release_index(ordinal_a);
  const int r_b = relaxed.release_index(ordinal_b);
  ASSERT_GT(r_a, 0);
  ASSERT_GT(r_b, 0);
  const size_t r_a_index = static_cast<size_t>(r_a);
  relaxed.zone.set_constraint(r_a_index, 0, 4);
  for (size_t j = 1; j < relaxed.zone.size(); ++j) {
    if (j != r_a_index) {
      relaxed.zone.set_constraint(r_a_index, j, 4);
    }
  }
  relaxed.zone.minimize();

  StateClass tied = relaxed;
  ASSERT_TRUE(tied.zone.tighten(static_cast<size_t>(r_a), static_cast<size_t>(r_b), 0));
  ASSERT_TRUE(tied.zone.is_consistent());
  EXPECT_EQ(release_bounds(tied, r_a), std::make_pair(0, 0));

  // Same marking, active set and clock layout: the only difference is the
  // arrival-order zone. The classes must not be merged.
  EXPECT_EQ(relaxed.marking, tied.marking);
  EXPECT_EQ(relaxed.clock_vars, tied.clock_vars);
  EXPECT_EQ(relaxed.release_clock_of_task, tied.release_clock_of_task);
  EXPECT_EQ(relaxed.priority_enabled, tied.priority_enabled);
  EXPECT_FALSE(state_class::check_equality(relaxed, tied));
  EXPECT_NE(state_class::hash_state_class(relaxed), state_class::hash_state_class(tied));

  StateClass copy = relaxed;
  EXPECT_TRUE(state_class::check_equality(relaxed, copy));
  EXPECT_EQ(state_class::hash_state_class(relaxed), state_class::hash_state_class(copy));
}

// ---------------------------------------------------------------------------
// 8. Unsupported models are rejected with clear errors
// ---------------------------------------------------------------------------

TEST(FifoTest, RejectsUnsupportedLoweredModels) {
  // No real core at all.
  expect_rejected(R"({
    "graph": {"name": "FifoNoCpu"},
    "configuration": {"num_cpus": 0, "cores_per_cpu": 1, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "num_cpus >= 1");

  // More than one running job per core.
  expect_rejected(R"({
    "graph": {"name": "FifoCoreCapacityTwo"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 2, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "cores_per_cpu == 1");

  // Queued jobs.
  expect_rejected(R"({
    "graph": {"name": "FifoCapacityTwo"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fifo",
                      "task_place_capacity": 2,
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "task_place_capacity == 1");

  // More than one initial job for a task.
  expect_rejected(R"({
    "graph": {"name": "FifoTwoStarts"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 2}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "at most one initial job");

  // Locks.
  expect_rejected(R"({
    "graph": {"name": "FifoLocks"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fifo",
                      "shared_locks": ["m"],
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 0,
               "time": [[1, 1]], "locks": ["m"]}],
    "edges": []
  })",
                  "does not include locks");

  // Dependency-driven release (task-to-task edge).
  expect_rejected(R"({
    "graph": {"name": "FifoDependency"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fifo",
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

  // A task on a core outside the declared range.
  expect_rejected(R"({
    "graph": {"name": "FifoCoreOne"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [{"id": "A", "type": "task", "priority": 0, "core": 1,
               "time": [[1, 1]], "locks": []}],
    "edges": []
  })",
                  "outside [0, 1)");

  // Fork/join.
  expect_rejected(R"({
    "graph": {"name": "FifoFork"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "policy": "fifo",
                      "start": [{"task": "A", "tokens": 1}]},
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0,
       "time": [[1, 1]], "locks": []},
      {"id": "F", "type": "fork", "core": 0, "time": [[1, 1]]}
    ],
    "edges": []
  })",
                  "fork/join");

  // Neither periodic nor started.
  expect_rejected(kFifoOneShotJson, "neither periodic");

  // A release that overlaps a running job is rejected at the successor state.
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

TEST(FifoTest, RejectsHandBuiltNetsAndReleaseClockReductions) {
  // No task metadata/chain.
  petri::PTPN hand_built;
  hand_built.add_transition("t", petri::TimeInterval(0, 0), 1, 0, false);
  try {
    FifoFiringRule rule(hand_built);
    FAIL() << "expected FIFO to reject a net without task chains";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("no task chains"), std::string::npos)
        << "unexpected message: " << error.what();
  }

  // FIFO has no scalar priority; a caller cannot silently fall back to one.
  FifoFiringRule unindexed;
  const PolicyContext context{hand_built, nullptr};
  EXPECT_THROW(unindexed.effective_priority(context, 0), std::logic_error);

  // k-extrapolation would relax the release-clock bounds.
  const petri::PTPN fifo_net = lower(parse_tdg(fifo_vs_fixed_json("fifo")));
  StateClassReachabilityGraph fifo_graph(fifo_net);
  EXPECT_THROW(fifo_graph.set_extrapolation(true), std::invalid_argument);

  // Installing FIFO after enabling extrapolation is rejected too.
  const petri::PTPN fixed_net = lower(parse_tdg(fifo_vs_fixed_json("fixed")));
  StateClassReachabilityGraph fixed_graph(fixed_net);
  fixed_graph.set_extrapolation(true);
  EXPECT_THROW(fixed_graph.set_firing_rule(std::make_unique<FifoFiringRule>()),
               std::invalid_argument);

  // Task symmetry cannot be applied to a single-core FIFO net; enabling it is
  // harmless and the graph builds without reducing the state space.
  StateClassReachabilityGraph symmetry_graph(fifo_net);
  symmetry_graph.set_symmetry_reduction(true);
  symmetry_graph.build(50000);
  EXPECT_FALSE(symmetry_graph.symmetry_applied());
  EXPECT_FALSE(symmetry_graph.get_statistics().truncated);
}

}  // namespace
