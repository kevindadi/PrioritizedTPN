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

  // Dynamic lock protocols are not implemented in this phase; they fall back
  // to the static rule and non-preemptive handling.
  for (const SchedulePolicy policy :
       {SchedulePolicy::PIP, SchedulePolicy::PCP, SchedulePolicy::SRP,
        SchedulePolicy::EDF, SchedulePolicy::LLF, SchedulePolicy::FIFO}) {
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


