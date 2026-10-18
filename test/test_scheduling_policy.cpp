#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "analysis/reachability/ptpn_analysis.h"
#include "analysis/reachability/scheduling.h"
#include "analysis/reachability/scheduling_policy.h"
#include "lower/tdg2pn/policy_priorities.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "lower/tdg2pn/tdg2pn_common.h"
#include "model/petri.h"
#include "model/tdg.h"

namespace {

using state_class::FiringRule;
using state_class::PolicyContext;
using state_class::PreemptionMode;
using state_class::Scheduling;
using state_class::StateClassReachabilityGraph;

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

// Position in the task's `node_pn_map` chain (0 = entry place).
size_t chain_place(const petri::PTPN& net, const std::string& task, size_t position) {
  const auto it = net.node_pn_map().find(task);
  EXPECT_NE(it, net.node_pn_map().end()) << task;
  if (it == net.node_pn_map().end() || position >= it->second.size()) {
    return SIZE_MAX;
  }
  EXPECT_TRUE(it->second[position].is_place()) << task << "[" << position << "]";
  return it->second[position].is_place() ? it->second[position].as_place().index() : SIZE_MAX;
}

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

state_class::MetricsReport analyze(const petri::PTPN& net) {
  StateClassReachabilityGraph graph(net);
  graph.build(5000);
  EXPECT_FALSE(graph.get_statistics().truncated);
  state_class::MetricsAnalyzer analyzer(graph.get_graph(), net, graph.get_initial_vertex(),
                                        /*exact=*/true);
  return analyzer.analyze();
}

const state_class::TaskMetrics& task_metrics(const state_class::MetricsReport& report,
                                             const std::string& name) {
  const auto it = std::find_if(report.tasks.begin(), report.tasks.end(),
                               [&](const state_class::TaskMetrics& task) {
                                 return task.name == name;
                               });
  EXPECT_NE(it, report.tasks.end()) << "missing task metrics for " << name;
  static const state_class::TaskMetrics kEmpty;
  return it == report.tasks.end() ? kEmpty : *it;
}

// Three periodic tasks, all on core 0. RM ranks (shortest period first):
// H=3, M=2, L=1.
const char* kRmEquivalenceJson = R"({
  "graph": {"name": "RmEquivalence"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "rm",
    "periodic": [
      {"task": "H", "period": 5},
      {"task": "M", "period": 10},
      {"task": "L", "period": 20}
    ]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 0, "core": 0, "time": [[1, 1]], "locks": []},
    {"id": "M", "type": "task", "priority": 0, "core": 0, "time": [[1, 2]], "locks": []},
    {"id": "L", "type": "task", "priority": 0, "core": 0, "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

const char* kRmEquivalentFixedJson = R"({
  "graph": {"name": "RmEquivalence"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "fixed",
    "periodic": [
      {"task": "H", "period": 5},
      {"task": "M", "period": 10},
      {"task": "L", "period": 20}
    ]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 3, "core": 0, "time": [[1, 1]], "locks": []},
    {"id": "M", "type": "task", "priority": 2, "core": 0, "time": [[1, 2]], "locks": []},
    {"id": "L", "type": "task", "priority": 1, "core": 0, "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

// DM ranks: explicit deadlines 2 < 4, then L's implicit deadline (= period 5).
// H=3, M=2, L=1 -- note this is the opposite of RM for H and L, because DM
// uses deadlines rather than release rates.
const char* kDmEquivalenceJson = R"({
  "graph": {"name": "DmEquivalence"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "dm",
    "periodic": [
      {"task": "H", "period": 20},
      {"task": "M", "period": 10},
      {"task": "L", "period": 5}
    ]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 0, "core": 0, "deadline": 2,
     "time": [[1, 1]], "locks": []},
    {"id": "M", "type": "task", "priority": 0, "core": 0, "deadline": 4,
     "time": [[1, 2]], "locks": []},
    {"id": "L", "type": "task", "priority": 0, "core": 0, "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

const char* kDmEquivalentFixedJson = R"({
  "graph": {"name": "DmEquivalence"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "fixed",
    "periodic": [
      {"task": "H", "period": 20},
      {"task": "M", "period": 10},
      {"task": "L", "period": 5}
    ]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 3, "core": 0, "deadline": 2,
     "time": [[1, 1]], "locks": []},
    {"id": "M", "type": "task", "priority": 2, "core": 0, "deadline": 4,
     "time": [[1, 2]], "locks": []},
    {"id": "L", "type": "task", "priority": 1, "core": 0, "time": [[2, 2]], "locks": []}
  ],
  "edges": []
})";

// Priority-inversion scenario. L starts at t=0 and acquires mutex1 during its
// long critical section; H and M are released every 15 time units. H (highest
// base priority) needs the same mutex; M is ready on the same core.
//
//   fixed: M preempts L's critical section -> H's response time is inflated.
//   pip:   L inherits H's priority while H waits -> M cannot preempt, and the
//          lock is released earlier.
std::string pip_inversion_json(const std::string& policy) {
  return R"({
    "graph": {"name": "PipInversion"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": ")" + policy + R"(",
      "shared_locks": ["mutex1"],
      "start": [{"task": "L", "tokens": 1}],
      "periodic": [
        {"task": "H", "period": 15},
        {"task": "M", "period": 15}
      ]
    },
    "nodes": [
      {"id": "H", "type": "task", "priority": 3, "core": 0,
       "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]},
      {"id": "M", "type": "task", "priority": 2, "core": 0,
       "time": [[2, 2]], "locks": []},
      {"id": "L", "type": "task", "priority": 1, "core": 0,
       "time": [[10, 10], [10, 10], [10, 10]], "locks": ["mutex1"]}
    ],
    "edges": []
  })";
}

// H (priority 3) and L (priority 1) share mutex1.
const char* kPipDirectJson = R"({
  "graph": {"name": "PipDirect"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pip",
    "shared_locks": ["mutex1"]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 3, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]},
    {"id": "L", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]}
  ],
  "edges": []
})";

// Nested locks: L holds lock1, M holds lock2 and waits for lock1, H waits for
// lock2. M's locks list is the acquisition order (lock2 first).
const char* kPipNestedJson = R"({
  "graph": {"name": "PipNested"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pip",
    "shared_locks": ["lock1", "lock2"]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 3, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["lock2"]},
    {"id": "M", "type": "task", "priority": 2, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1], [1, 1], [1, 1]],
     "locks": ["lock2", "lock1"]},
    {"id": "L", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["lock1"]}
  ],
  "edges": []
})";

// Two independent locks, each held by a low-priority task while an equal-
// priority (5) high-priority task waits.
const char* kPipTieJson = R"({
  "graph": {"name": "PipTie"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pip",
    "shared_locks": ["m1", "m2"]
  },
  "nodes": [
    {"id": "B1", "type": "task", "priority": 5, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["m1"]},
    {"id": "B2", "type": "task", "priority": 5, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["m2"]},
    {"id": "A1", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["m1"]},
    {"id": "A2", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["m2"]}
  ],
  "edges": []
})";

// Two tasks hold the same lock in the same marking (impossible for a real
// capacity-1 lock place, exercised to verify the conservative fallback).
const char* kPipTwoHoldersJson = R"({
  "graph": {"name": "PipTwoHolders"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pip",
    "shared_locks": ["mutex1"]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 3, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]},
    {"id": "A", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]},
    {"id": "B", "type": "task", "priority": 2, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]}
  ],
  "edges": []
})";

// PIP requires task_place_capacity == 1; queued job tokens would make the
// chain-position ownership inference ambiguous.
const char* kPipCapacityTwoJson = R"({
  "graph": {"name": "PipCapacityTwo"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pip",
    "task_place_capacity": 2,
    "shared_locks": ["mutex1"]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 3, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]},
    {"id": "L", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]}
  ],
  "edges": []
})";

// Test double: drops one transition and overrides one transition's priority.
class TestFiringRule : public FiringRule {
 public:
  TestFiringRule(size_t filtered, size_t boosted, int boosted_priority)
      : filtered_(filtered), boosted_(boosted), boosted_priority_(boosted_priority) {}

  bool admissible(const PolicyContext& /*context*/, size_t transition) const override {
    return transition != filtered_;
  }

  int effective_priority(const PolicyContext& context, size_t transition) const override {
    if (transition == boosted_) {
      return boosted_priority_;
    }
    return context.net.get_transition(petri::TransitionId{transition}).priority;
  }

  PreemptionMode preemption_mode() const override {
    return PreemptionMode::RESUME;
  }

 private:
  size_t filtered_;
  size_t boosted_;
  int boosted_priority_;
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. RM priority groups
// ---------------------------------------------------------------------------

TEST(SchedulingPolicyTest, RmPrioritiesFollowPeriods) {
  const tdg::TDG tdg = parse_tdg(R"({
    "graph": {"name": "RmGroups"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "rm",
      "periodic": [
        {"task": "A", "period": 20},
        {"task": "B", "period": 10},
        {"task": "C", "period": 5},
        {"task": "D", "period": 10}
      ]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 900, "core": 0, "time": [[1, 1]], "locks": []},
      {"id": "B", "type": "task", "priority": 0, "core": 0, "time": [[1, 1]], "locks": []},
      {"id": "C", "type": "task", "priority": 0, "core": 0, "time": [[1, 1]], "locks": []},
      {"id": "D", "type": "task", "priority": 0, "core": 0, "time": [[1, 1]], "locks": []},
      {"id": "E", "type": "task", "priority": 42, "core": 0, "time": [[1, 1]], "locks": []}
    ],
    "edges": []
  })");

  const auto priorities = converter::detail::compute_policy_priorities(tdg);
  ASSERT_EQ(priorities.size(), 5u);

  // Shorter period -> strictly higher priority; equal periods tie.
  EXPECT_GT(priorities.at("C"), priorities.at("B"));
  EXPECT_GT(priorities.at("B"), priorities.at("A"));
  EXPECT_EQ(priorities.at("B"), priorities.at("D"));

  // Aperiodic tasks get the lowest priority; explicit input priorities are
  // ignored.
  EXPECT_EQ(priorities.at("E"), 0);
  EXPECT_NE(priorities.at("A"), 900);

  // apply_policy_priorities writes every representation.
  tdg::TDG applied = tdg;
  converter::detail::apply_policy_priorities(applied);
  EXPECT_EQ(applied.tasks_priority.at("A"), priorities.at("A"));
  EXPECT_EQ(applied.tasks_priority.at("C"), priorities.at("C"));
  EXPECT_EQ(applied.tasks_priority.at("E"), 0);

  for (const auto& node : applied.all_task) {
    if (const auto* task = as_task_node(node)) {
      EXPECT_EQ(task->priority, priorities.at(task->name)) << task->name;
    }
  }
  for (const auto& [name, node] : applied.nodes_type) {
    if (const auto* task = as_task_node(node)) {
      EXPECT_EQ(task->priority, priorities.at(name)) << name;
    }
  }
}

// ---------------------------------------------------------------------------
// 2. DM priority groups
// ---------------------------------------------------------------------------

TEST(SchedulingPolicyTest, DmPrioritiesFollowDeadlinesThenPeriods) {
  const tdg::TDG tdg = parse_tdg(R"({
    "graph": {"name": "DmGroups"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "dm",
      "periodic": [
        {"task": "A", "period": 100},
        {"task": "B", "period": 90},
        {"task": "C", "period": 80},
        {"task": "D", "period": 5},
        {"task": "F", "period": 2}
      ]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 0, "core": 0, "deadline": 8,
       "time": [[1, 1]], "locks": []},
      {"id": "B", "type": "task", "priority": 0, "core": 0, "deadline": 4,
       "time": [[1, 1]], "locks": []},
      {"id": "C", "type": "task", "priority": 0, "core": 0, "deadline": 2,
       "time": [[1, 1]], "locks": []},
      {"id": "D", "type": "task", "priority": 0, "core": 0, "time": [[1, 1]], "locks": []},
      {"id": "F", "type": "task", "priority": 0, "core": 0, "deadline": 50,
       "time": [[1, 1]], "locks": []},
      {"id": "E", "type": "task", "priority": 7, "core": 0, "time": [[1, 1]], "locks": []}
    ],
    "edges": []
  })");

  const auto priorities = converter::detail::compute_policy_priorities(tdg);
  ASSERT_EQ(priorities.size(), 6u);

  // Explicit deadlines (2 < 4 < 8) outrank periods.
  EXPECT_GT(priorities.at("C"), priorities.at("B"));
  EXPECT_GT(priorities.at("B"), priorities.at("A"));

  // D has no deadline and falls back to its period (5).
  EXPECT_GT(priorities.at("D"), priorities.at("A"));

  // F has a very short period but a long deadline (50): DM uses the deadline,
  // so F ranks below every task with a shorter key.
  EXPECT_LT(priorities.at("F"), priorities.at("A"));
  EXPECT_LT(priorities.at("F"), priorities.at("D"));

  // Neither period nor deadline -> lowest priority; explicit priority ignored.
  EXPECT_EQ(priorities.at("E"), 0);
  EXPECT_NE(priorities.at("A"), 0);
}

// ---------------------------------------------------------------------------
// 3. Equivalence with explicit fixed priorities
// ---------------------------------------------------------------------------

TEST(SchedulingPolicyTest, RmMatchesFixedWithEquivalentPriorities) {
  const tdg::TDG rm_tdg = parse_tdg(kRmEquivalenceJson);
  const tdg::TDG fixed_tdg = parse_tdg(kRmEquivalentFixedJson);
  const petri::PTPN rm_net = lower(rm_tdg);
  const petri::PTPN fixed_net = lower(fixed_tdg);

  ASSERT_EQ(rm_net.num_places(), fixed_net.num_places());
  ASSERT_EQ(rm_net.num_transitions(), fixed_net.num_transitions());
  for (size_t t = 0; t < rm_net.num_transitions(); ++t) {
    const auto& rm_t = rm_net.get_transition(petri::TransitionId{t});
    const auto& fixed_t = fixed_net.get_transition(petri::TransitionId{t});
    ASSERT_EQ(rm_t.name, fixed_t.name);
    EXPECT_EQ(rm_t.priority, fixed_t.priority) << rm_t.name;
  }

  const auto rm_report = analyze(rm_net);
  const auto fixed_report = analyze(fixed_net);
  EXPECT_EQ(rm_report.states, fixed_report.states);
  EXPECT_EQ(rm_report.transitions, fixed_report.transitions);
  EXPECT_GT(rm_report.states, 1u);

  for (const std::string name : {"H", "M", "L"}) {
    const auto& rm_task = task_metrics(rm_report, name);
    const auto& fixed_task = task_metrics(fixed_report, name);
    EXPECT_TRUE(rm_task.observed) << name;
    EXPECT_TRUE(fixed_task.observed) << name;
    EXPECT_EQ(rm_task.priority, fixed_task.priority) << name;
    EXPECT_EQ(rm_task.wcrt.infinite, fixed_task.wcrt.infinite) << name;
    EXPECT_EQ(rm_task.wcrt.value, fixed_task.wcrt.value) << name;
  }
}

TEST(SchedulingPolicyTest, DmMatchesFixedWithEquivalentPriorities) {
  const tdg::TDG dm_tdg = parse_tdg(kDmEquivalenceJson);
  const tdg::TDG fixed_tdg = parse_tdg(kDmEquivalentFixedJson);
  const petri::PTPN dm_net = lower(dm_tdg);
  const petri::PTPN fixed_net = lower(fixed_tdg);

  ASSERT_EQ(dm_net.num_places(), fixed_net.num_places());
  ASSERT_EQ(dm_net.num_transitions(), fixed_net.num_transitions());
  for (size_t t = 0; t < dm_net.num_transitions(); ++t) {
    const auto& dm_t = dm_net.get_transition(petri::TransitionId{t});
    const auto& fixed_t = fixed_net.get_transition(petri::TransitionId{t});
    ASSERT_EQ(dm_t.name, fixed_t.name);
    EXPECT_EQ(dm_t.priority, fixed_t.priority) << dm_t.name;
  }

  const auto dm_report = analyze(dm_net);
  const auto fixed_report = analyze(fixed_net);
  EXPECT_EQ(dm_report.states, fixed_report.states);
  EXPECT_EQ(dm_report.transitions, fixed_report.transitions);
  EXPECT_GT(dm_report.states, 1u);

  for (const std::string name : {"H", "M", "L"}) {
    const auto& dm_task = task_metrics(dm_report, name);
    const auto& fixed_task = task_metrics(fixed_report, name);
    EXPECT_TRUE(dm_task.observed) << name;
    EXPECT_TRUE(fixed_task.observed) << name;
    EXPECT_EQ(dm_task.priority, fixed_task.priority) << name;
    EXPECT_EQ(dm_task.wcrt.infinite, fixed_task.wcrt.infinite) << name;
    EXPECT_EQ(dm_task.wcrt.value, fixed_task.wcrt.value) << name;
  }
}

// ---------------------------------------------------------------------------
// 4. Default fixed rule
// ---------------------------------------------------------------------------

TEST(SchedulingPolicyTest, FixedRuleIsStaticAndResumes) {
  petri::PTPN net;
  net.add_transition("low", petri::TimeInterval(0, 1), 1, 0, false);
  net.add_transition("high", petri::TimeInterval(0, 1), 9, 0, false);

  auto rule = state_class::make_firing_rule(SchedulePolicy::FIXED);
  ASSERT_NE(rule, nullptr);
  const PolicyContext context{net, nullptr};
  EXPECT_EQ(rule->effective_priority(context, 0), 1);
  EXPECT_EQ(rule->effective_priority(context, 1), 9);
  EXPECT_TRUE(rule->admissible(context, 0));
  EXPECT_EQ(rule->preemption_mode(), PreemptionMode::RESUME);

  // The metadata default for a directly built/parsed net is FIXED.
  petri::PTPN direct;
  EXPECT_EQ(direct.schedule_policy(), SchedulePolicy::FIXED);
}

TEST(SchedulingPolicyTest, PolicyPreemptionModesMatchCurrentCoverage) {
  petri::PTPN net;
  const PolicyContext context{net, nullptr};
  EXPECT_EQ(state_class::make_firing_rule(SchedulePolicy::FIXED_PRIOR_WITH_RESUME)
                ->preemption_mode(),
            PreemptionMode::RESUME);
  EXPECT_EQ(state_class::make_firing_rule(SchedulePolicy::RM)->preemption_mode(),
            PreemptionMode::RESUME);
  EXPECT_EQ(state_class::make_firing_rule(SchedulePolicy::DM)->preemption_mode(),
            PreemptionMode::RESUME);
  EXPECT_EQ(state_class::make_firing_rule(SchedulePolicy::FIXED_PRIOR_WITH_RESTART)
                ->preemption_mode(),
            PreemptionMode::RESTART);

  // PIP is analyzed dynamically and uses resume-style preemption.
  auto pip_rule = state_class::make_firing_rule(SchedulePolicy::PIP);
  ASSERT_NE(pip_rule, nullptr);
  EXPECT_NE(dynamic_cast<state_class::PipFiringRule*>(pip_rule.get()), nullptr);
  EXPECT_EQ(pip_rule->preemption_mode(), PreemptionMode::RESUME);
  EXPECT_TRUE(pip_rule->admissible(context, 0));
  EXPECT_TRUE(converter::detail::is_resume_policy(SchedulePolicy::PIP));

  // Unimplemented policies fall back to the static rule and non-preemptive
  // handling.
  for (const SchedulePolicy policy :
       {SchedulePolicy::PCP, SchedulePolicy::SRP, SchedulePolicy::EDF,
        SchedulePolicy::LLF, SchedulePolicy::FIFO}) {
    auto rule = state_class::make_firing_rule(policy);
    EXPECT_EQ(rule->preemption_mode(), PreemptionMode::NON_PREEMPTIVE);
    EXPECT_TRUE(rule->admissible(context, 0));
  }
}

// ---------------------------------------------------------------------------
// 5. Generic branch enumeration honors the rule
// ---------------------------------------------------------------------------

TEST(SchedulingPolicyTest, PriorityBranchesUseRuleAdmissibilityAndPriorities) {
  petri::PTPN net;
  // t0=5, t1=7, t2=7, t3=1 on one real core (capacity defaults to 1).
  net.add_transition("t0", petri::TimeInterval(0, 1), 5, 0, false);
  net.add_transition("t1", petri::TimeInterval(0, 1), 7, 0, false);
  net.add_transition("t2", petri::TimeInterval(0, 1), 7, 0, false);
  net.add_transition("t3", petri::TimeInterval(0, 1), 1, 0, false);

  const PolicyContext context{net, nullptr};
  const state_class::TransitionSet enabled = {0, 1, 2, 3};

  // Static rule: t1 and t2 tie at the top -> both tie resolutions enumerated.
  auto static_rule = state_class::make_firing_rule(SchedulePolicy::FIXED);
  const auto static_branches =
      Scheduling::priority_branches(enabled, net, *static_rule, context);
  ASSERT_EQ(static_branches.size(), 2u);
  EXPECT_EQ(static_branches[0], (state_class::TransitionSet{1}));
  EXPECT_EQ(static_branches[1], (state_class::TransitionSet{2}));

  // Custom rule: filter t1 out and boost t0 from 5 to 7. t0 and t2 now tie;
  // t1 must not appear even though its static priority was maximal.
  const TestFiringRule custom_rule(/*filtered=*/1, /*boosted=*/0, /*boosted_priority=*/7);
  const auto custom_branches =
      Scheduling::priority_branches(enabled, net, custom_rule, context);
  ASSERT_EQ(custom_branches.size(), 2u);
  EXPECT_EQ(custom_branches[0], (state_class::TransitionSet{0}));
  EXPECT_EQ(custom_branches[1], (state_class::TransitionSet{2}));

  for (const auto& branch : custom_branches) {
    EXPECT_EQ(std::find(branch.begin(), branch.end(), 1u), branch.end());
  }
}

TEST(SchedulingPolicyTest, PriorityBranchesEnumerateControlCoreMaximalSet) {
  petri::PTPN net;
  net.add_transition("c0", petri::TimeInterval(0, 1), 5, -1, false);
  net.add_transition("c1", petri::TimeInterval(0, 1), 5, -1, false);
  net.add_transition("c2", petri::TimeInterval(0, 1), 1, -1, false);

  auto rule = state_class::make_firing_rule(SchedulePolicy::FIXED);
  const PolicyContext context{net, nullptr};
  const auto branches =
      Scheduling::priority_branches({0, 1, 2}, net, *rule, context);
  ASSERT_EQ(branches.size(), 1u);
  EXPECT_EQ(branches.front(), (state_class::TransitionSet{0, 1}));
}

// ---------------------------------------------------------------------------
// 6. RM/DM lowering: resume resources and computed TaskInfo priorities
// ---------------------------------------------------------------------------

TEST(SchedulingPolicyTest, RmLoweringUsesResumeResourcesAndComputedPriorities) {
  tdg::TDG tdg = parse_tdg(kRmEquivalenceJson);
  const petri::PTPN net = lower(tdg);

  EXPECT_EQ(net.schedule_policy(), SchedulePolicy::RM);

  // Resume-style lowering: no structural core resource places, and execution
  // segments are suspendable.
  EXPECT_EQ(find_place(net, "core0"), SIZE_MAX);
  const size_t h_exec = find_transition(net, "Hexec");
  ASSERT_NE(h_exec, SIZE_MAX);
  EXPECT_TRUE(net.get_transition(petri::TransitionId{h_exec}).suspendable);

  EXPECT_EQ(net.task_info().at("H").priority, 3);
  EXPECT_EQ(net.task_info().at("M").priority, 2);
  EXPECT_EQ(net.task_info().at("L").priority, 1);

  // Lowering must not mutate the caller's TDG (priorities stay as supplied).
  const auto* h_task = [&]() -> const TaskNode* {
    for (const auto& node : tdg.all_task) {
      const auto* task = as_task_node(node);
      if (task != nullptr && task->name == "H") {
        return task;
      }
    }
    return nullptr;
  }();
  ASSERT_NE(h_task, nullptr);
  EXPECT_EQ(h_task->priority, 0);
}

TEST(SchedulingPolicyTest, DmLoweringUsesResumeResourcesAndComputedPriorities) {
  const std::string dm_json = R"({
    "graph": {"name": "DmLowering"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "dm",
      "periodic": [
        {"task": "H", "period": 20},
        {"task": "M", "period": 10},
        {"task": "L", "period": 5}
      ]
    },
    "nodes": [
      {"id": "H", "type": "task", "priority": 99, "core": 0, "deadline": 2,
       "time": [[1, 1]], "locks": []},
      {"id": "M", "type": "task", "priority": 99, "core": 0, "deadline": 4,
       "time": [[1, 1]], "locks": []},
      {"id": "L", "type": "task", "priority": 99, "core": 0,
       "time": [[1, 1]], "locks": []}
    ],
    "edges": []
  })";

  const tdg::TDG tdg = parse_tdg(dm_json);
  const petri::PTPN net = lower(tdg);

  EXPECT_EQ(net.schedule_policy(), SchedulePolicy::DM);
  EXPECT_EQ(find_place(net, "core0"), SIZE_MAX);
  const size_t l_exec = find_transition(net, "Lexec");
  ASSERT_NE(l_exec, SIZE_MAX);
  EXPECT_TRUE(net.get_transition(petri::TransitionId{l_exec}).suspendable);

  EXPECT_EQ(net.task_info().at("H").priority, 3);
  EXPECT_EQ(net.task_info().at("M").priority, 2);
  EXPECT_EQ(net.task_info().at("L").priority, 1);
}

// ---------------------------------------------------------------------------
// 7. PIP: dynamic priority inheritance
// ---------------------------------------------------------------------------

TEST(PipPolicyTest, PriorityInheritanceReducesHighPriorityWcrt) {
  const tdg::TDG fixed_tdg = parse_tdg(pip_inversion_json("fixed"));
  const tdg::TDG pip_tdg = parse_tdg(pip_inversion_json("pip"));
  const petri::PTPN fixed_net = lower(fixed_tdg);
  const petri::PTPN pip_net = lower(pip_tdg);

  ASSERT_EQ(fixed_net.schedule_policy(), SchedulePolicy::FIXED);
  ASSERT_EQ(pip_net.schedule_policy(), SchedulePolicy::PIP);

  const auto fixed_report = analyze(fixed_net);
  const auto pip_report = analyze(pip_net);

  const auto& fixed_h = task_metrics(fixed_report, "H");
  const auto& pip_h = task_metrics(pip_report, "H");
  EXPECT_TRUE(fixed_h.observed);
  EXPECT_TRUE(pip_h.observed);
  ASSERT_FALSE(fixed_h.wcrt.infinite);
  ASSERT_FALSE(pip_h.wcrt.infinite);

  // L inherits H's priority while H waits on the mutex, so M cannot preempt
  // L's critical section and H finishes earlier.
  EXPECT_GT(fixed_h.wcrt.value, pip_h.wcrt.value);
  EXPECT_EQ(fixed_h.wcrt.value, 10);
  EXPECT_EQ(pip_h.wcrt.value, 8);

  // Both policies keep the medium-priority task schedulable and observed.
  EXPECT_TRUE(task_metrics(fixed_report, "M").observed);
  EXPECT_TRUE(task_metrics(pip_report, "M").observed);
}

TEST(PipPolicyTest, EffectivePriorityFollowsMarkingAndReverts) {
  const petri::PTPN net = lower(parse_tdg(kPipDirectJson));
  const state_class::PipFiringRule rule(net);

  const size_t acquire = state_class::PipFiringRule::kLockAcquireBase;
  const size_t l_hold = chain_place(net, "L", acquire + 1);
  const size_t h_pre = chain_place(net, "H", acquire - 1);
  const size_t l_crit = chain_transition(net, "L", acquire + 2);
  const size_t h_acquire = chain_transition(net, "H", acquire);
  const size_t l_consume = find_transition(net, "L_consume");
  const size_t lock = net.lock_places().at("mutex1").index();
  ASSERT_NE(l_hold, SIZE_MAX);
  ASSERT_NE(h_pre, SIZE_MAX);
  ASSERT_NE(l_crit, SIZE_MAX);
  ASSERT_NE(h_acquire, SIZE_MAX);
  ASSERT_NE(l_consume, SIZE_MAX);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  state.marking[l_hold] = 1;  // L holds mutex1
  state.marking[h_pre] = 1;   // H waits on the (empty) lock place
  const PolicyContext context{net, &state};

  // H (base 3) waits for L's lock: L's inherited effective priority is 3.
  EXPECT_EQ(rule.effective_priority(context, l_crit), 3);
  EXPECT_EQ(rule.effective_priority(context, h_acquire), 3);

  // Reversion: once the lock is free again, H is no longer blocked and L
  // returns to its base priority.
  state.marking[lock] = 1;
  EXPECT_EQ(rule.effective_priority(context, l_crit), 1);

  // No waiter at all: still the base priority.
  state.marking[lock] = 0;
  state.marking[h_pre] = 0;
  EXPECT_EQ(rule.effective_priority(context, l_crit), 1);
  EXPECT_EQ(rule.effective_priority(context, h_acquire), 3);

  // Control transitions keep their static priority (0 for consume).
  EXPECT_EQ(rule.effective_priority(context, l_consume), 0);
}

TEST(PipPolicyTest, TransitiveInheritancePropagatesThroughHolderChain) {
  const petri::PTPN net = lower(parse_tdg(kPipNestedJson));
  const state_class::PipFiringRule rule(net);

  const size_t acquire = state_class::PipFiringRule::kLockAcquireBase;
  const size_t stride = state_class::PipFiringRule::kLockStride;
  const size_t l_hold1 = chain_place(net, "L", acquire + 1);
  const size_t h_pre2 = chain_place(net, "H", acquire - 1);
  const size_t m_hold2 = chain_place(net, "M", acquire + 1);
  const size_t m_pre1 = chain_place(net, "M", acquire + stride - 1);
  const size_t m_acquire1 = chain_transition(net, "M", acquire + stride);
  const size_t l_crit = chain_transition(net, "L", acquire + 2);

  ASSERT_NE(l_hold1, SIZE_MAX);
  ASSERT_NE(h_pre2, SIZE_MAX);
  ASSERT_NE(m_hold2, SIZE_MAX);
  ASSERT_NE(m_pre1, SIZE_MAX);
  ASSERT_NE(m_acquire1, SIZE_MAX);
  ASSERT_NE(l_crit, SIZE_MAX);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  // Reachable snapshot with exactly one token per task chain:
  //   L: token at its first hold place -> holds lock1.
  //   M: token past its first hold place, at the second acquire's pre-place.
  //      Position-range ownership still sees lock2 as held.
  //   H: token at the acquire pre-place of lock2 -> waits for lock2.
  state.marking[l_hold1] = 1;
  state.marking[m_pre1] = 1;
  state.marking[h_pre2] = 1;
  const PolicyContext context{net, &state};

  // M's first hold place is empty; ownership comes from the token range.
  EXPECT_EQ(state.marking[m_hold2], 0);

  // H (3) waits for lock2 held by M -> M inherits 3; M waits for lock1
  // held by L -> L inherits M's effective priority 3.
  EXPECT_EQ(rule.effective_priority(context, m_acquire1), 3);
  EXPECT_EQ(rule.effective_priority(context, l_crit), 3);

  // With H no longer waiting, inheritance still propagates one hop:
  // M waits for L, so L inherits M's base priority 2 (not H's 3).
  state.marking[h_pre2] = 0;
  EXPECT_EQ(rule.effective_priority(context, m_acquire1), 2);
  EXPECT_EQ(rule.effective_priority(context, l_crit), 2);

  // Move M's token back to its first hold place: M still owns lock2 but no
  // longer waits, so L returns to its base priority.
  state.marking[m_pre1] = 0;
  state.marking[m_hold2] = 1;
  EXPECT_EQ(rule.effective_priority(context, m_acquire1), 2);
  EXPECT_EQ(rule.effective_priority(context, l_crit), 1);
}

TEST(PipPolicyTest, MultiLockRangeOwnershipCoversWholeCriticalSection) {
  const petri::PTPN net = lower(parse_tdg(kPipNestedJson));
  const state_class::PipFiringRule rule(net);

  const size_t acquire = state_class::PipFiringRule::kLockAcquireBase;
  const size_t stride = state_class::PipFiringRule::kLockStride;
  const size_t m_chain_length = net.node_pn_map().at("M").size();
  const size_t release0 =
      m_chain_length - state_class::PipFiringRule::kLockReleaseSuffix;

  const size_t h_pre2 = chain_place(net, "H", acquire - 1);
  const size_t l_hold1 = chain_place(net, "L", acquire + 1);
  const size_t m_hold2 = chain_place(net, "M", acquire + 1);
  const size_t m_waiting1 = chain_place(net, "M", acquire + stride - 1);
  const size_t m_before_release = chain_place(net, "M", release0 - 1);
  const size_t m_after_release = chain_place(net, "M", release0 + 1);
  const size_t m_probe = chain_transition(net, "M", acquire + 2);
  const size_t l_probe = chain_transition(net, "L", acquire + 2);

  ASSERT_NE(h_pre2, SIZE_MAX);
  ASSERT_NE(l_hold1, SIZE_MAX);
  ASSERT_NE(m_hold2, SIZE_MAX);
  ASSERT_NE(m_waiting1, SIZE_MAX);
  ASSERT_NE(m_before_release, SIZE_MAX);
  ASSERT_NE(m_after_release, SIZE_MAX);
  ASSERT_NE(m_probe, SIZE_MAX);
  ASSERT_NE(l_probe, SIZE_MAX);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  state.marking[h_pre2] = 1;  // H waits for lock2
  state.marking[l_hold1] = 1;  // L holds lock1

  // M has already left its first hold place (position 6) and is at the
  // second acquire pre-place (position 8), inside lock2's ownership range.
  state.marking[m_waiting1] = 1;
  EXPECT_EQ(state.marking[m_hold2], 0);
  const PolicyContext context{net, &state};
  EXPECT_EQ(rule.effective_priority(context, m_probe), 3);
  EXPECT_EQ(rule.effective_priority(context, l_probe), 3);

  // Position release-1 is the release transition's pre-place: still owned.
  state.marking[m_waiting1] = 0;
  state.marking[m_before_release] = 1;
  EXPECT_EQ(rule.effective_priority(context, m_probe), 3);
  // M no longer waits for lock1, so L keeps its base priority.
  EXPECT_EQ(rule.effective_priority(context, l_probe), 1);

  // Position release+1 is after the release transition: no longer owned.
  // The lock place deliberately stays empty so H keeps waiting; this direct
  // marking is inconsistent at the boundary and only exercises the range.
  state.marking[m_before_release] = 0;
  state.marking[m_after_release] = 1;
  EXPECT_EQ(rule.effective_priority(context, m_probe), 2);
  EXPECT_EQ(rule.effective_priority(context, l_probe), 1);
}

TEST(PipPolicyTest, LoweringRejectsPipWithTaskPlaceCapacityAboveOne) {
  const tdg::TDG tdg = parse_tdg(kPipCapacityTwoJson);
  try {
    lower(tdg);
    FAIL() << "expected lowering to reject task_place_capacity > 1 for pip";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("task_place_capacity"),
              std::string::npos);
  }

  // The default capacity of 1 remains accepted.
  EXPECT_NO_THROW(lower(parse_tdg(kPipDirectJson)));
}

TEST(PipPolicyTest, LoweringRejectsMultipleInitialJobs) {
  const std::string json = R"({
    "graph": {"name": "PipStartTokens"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "pip",
      "start": [{"task": "H", "tokens": 2}],
      "shared_locks": ["mutex1"]
    },
    "nodes": [
      {"id": "H", "type": "task", "priority": 3, "core": 0,
       "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutex1"]}
    ],
    "edges": []
  })";
  try {
    lower(parse_tdg(json));
    FAIL() << "expected lowering to reject multiple initial PIP jobs";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("one initial job"),
              std::string::npos);
  }
}

TEST(PipPolicyTest, EqualInheritedPrioritiesEnumerateBothBranches) {
  const petri::PTPN net = lower(parse_tdg(kPipTieJson));
  const state_class::PipFiringRule rule(net);

  const size_t acquire = state_class::PipFiringRule::kLockAcquireBase;
  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  // A1/A2 each hold one lock; B1/B2 wait for the respective lock.
  state.marking[chain_place(net, "A1", acquire + 1)] = 1;
  state.marking[chain_place(net, "A2", acquire + 1)] = 1;
  state.marking[chain_place(net, "B1", acquire - 1)] = 1;
  state.marking[chain_place(net, "B2", acquire - 1)] = 1;
  const PolicyContext context{net, &state};

  const size_t a1_crit = chain_transition(net, "A1", acquire + 2);
  const size_t a2_crit = chain_transition(net, "A2", acquire + 2);
  ASSERT_NE(a1_crit, SIZE_MAX);
  ASSERT_NE(a2_crit, SIZE_MAX);
  EXPECT_EQ(rule.effective_priority(context, a1_crit), 5);
  EXPECT_EQ(rule.effective_priority(context, a2_crit), 5);

  // One core, capacity 1: both inherited-priority candidates are maximal and
  // both tie resolutions must be present.
  const auto branches =
      Scheduling::priority_branches({a1_crit, a2_crit}, net, rule, context);
  ASSERT_EQ(branches.size(), 2u);
  // Branch order follows transition indices, which need not match task order.
  EXPECT_EQ(branches[0],
            (state_class::TransitionSet{std::min(a1_crit, a2_crit)}));
  EXPECT_EQ(branches[1],
            (state_class::TransitionSet{std::max(a1_crit, a2_crit)}));
}

TEST(PipPolicyTest, AmbiguousChainsAndMultipleHoldersFallBackToBase) {
  // (a) Two job tokens in H's chain (a queued release): the chain position is
  // ambiguous, so the state is unsupported for H and no inheritance is
  // inferred; L stays at its base priority.
  const petri::PTPN direct = lower(parse_tdg(kPipDirectJson));
  const state_class::PipFiringRule direct_rule(direct);
  const size_t acquire = state_class::PipFiringRule::kLockAcquireBase;
  const size_t h_entry = chain_place(direct, "H", 0);
  state_class::StateClass state;
  state.marking.assign(direct.num_places(), 0);
  state.marking[chain_place(direct, "L", acquire + 1)] = 1;
  state.marking[chain_place(direct, "H", acquire - 1)] = 1;
  state.marking[h_entry] = 1;  // second in-flight job token
  const PolicyContext direct_context{direct, &state};
  EXPECT_EQ(direct_rule.effective_priority(
                direct_context, chain_transition(direct, "L", acquire + 2)),
            1);
  EXPECT_EQ(direct_rule.effective_priority(
                direct_context, chain_transition(direct, "H", acquire)),
            3);

  // (b) Two holders for the same lock: propagation through that lock is
  // disabled, so both holders keep their base priorities.
  const petri::PTPN two = lower(parse_tdg(kPipTwoHoldersJson));
  const state_class::PipFiringRule two_rule(two);
  state_class::StateClass two_state;
  two_state.marking.assign(two.num_places(), 0);
  two_state.marking[chain_place(two, "A", acquire + 1)] = 1;
  two_state.marking[chain_place(two, "B", acquire + 1)] = 1;
  two_state.marking[chain_place(two, "H", acquire - 1)] = 1;
  const PolicyContext two_context{two, &two_state};
  EXPECT_EQ(two_rule.effective_priority(two_context, chain_transition(two, "A", acquire + 2)),
            1);
  EXPECT_EQ(two_rule.effective_priority(two_context, chain_transition(two, "B", acquire + 2)),
            2);
  EXPECT_EQ(two_rule.effective_priority(two_context, chain_transition(two, "H", acquire)), 3);
}

TEST(PipPolicyTest, PipUsesResumeLoweringAndNetAwareRule) {
  EXPECT_TRUE(converter::detail::is_resume_policy(SchedulePolicy::PIP));

  const petri::PTPN net = lower(parse_tdg(kPipDirectJson));
  ASSERT_EQ(net.schedule_policy(), SchedulePolicy::PIP);

  // Resume-style lowering: no structural core place, suspendable execution
  // segments.
  EXPECT_EQ(find_place(net, "core0"), SIZE_MAX);
  const size_t h_exec = find_transition(net, "H_exec_2");
  ASSERT_NE(h_exec, SIZE_MAX);
  EXPECT_TRUE(net.get_transition(petri::TransitionId{h_exec}).suspendable);

  // The net-aware factory precomputes PIP's index. The policy-only factory
  // builds it lazily from PolicyContext::net and behaves identically.
  auto net_rule = state_class::make_firing_rule(SchedulePolicy::PIP, net);
  auto lazy_rule = state_class::make_firing_rule(SchedulePolicy::PIP);
  ASSERT_NE(dynamic_cast<state_class::PipFiringRule*>(net_rule.get()), nullptr);
  ASSERT_NE(dynamic_cast<state_class::PipFiringRule*>(lazy_rule.get()), nullptr);
  EXPECT_EQ(net_rule->preemption_mode(), PreemptionMode::RESUME);
  EXPECT_EQ(lazy_rule->preemption_mode(), PreemptionMode::RESUME);

  const size_t acquire = state_class::PipFiringRule::kLockAcquireBase;
  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  state.marking[chain_place(net, "L", acquire + 1)] = 1;
  state.marking[chain_place(net, "H", acquire - 1)] = 1;
  const PolicyContext context{net, &state};
  const size_t l_crit = chain_transition(net, "L", acquire + 2);
  EXPECT_EQ(net_rule->effective_priority(context, l_crit), 3);
  EXPECT_EQ(lazy_rule->effective_priority(context, l_crit), 3);
}


