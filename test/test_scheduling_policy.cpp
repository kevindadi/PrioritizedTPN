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

// ---------------------------------------------------------------------------
// PCP fixtures
// ---------------------------------------------------------------------------

// H and L share mutex1 (direct blocking); used by the direct rule tests.
const char* kPcpDirectJson = R"({
  "graph": {"name": "PcpDirect"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pcp",
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

// Ceiling blocking of a medium task that wants a *different* free lock.
//
//   L (priority 1) starts at t=0 and holds mutexA from t=4 to t=26; its long
//   critical section spans H's first release at t=15.
//   D2 (priority 99, zero-time) releases H (priority 3) once at t=15.
//   D (priority 99, zero-time) completes immediately and its edge releases M
//   (priority 2) at t=5, before H.
//   M wants mutexB, which is free. ceiling(mutexA)=3 (H and L use it), so under
//   PCP M's acquire is inadmissible while L holds mutexA. Under fixed/PIP M
//   acquires mutexB and preempts L, delaying H.
//   H waits for mutexA held by L.
std::string pcp_ceiling_json(const std::string& policy) {
  return R"({
    "graph": {"name": "PcpCeilingBlock"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": ")" + policy + R"(",
      "shared_locks": ["mutexA", "mutexB"],
      "start": [{"task": "L", "tokens": 1}, {"task": "D", "tokens": 1},
                {"task": "D2", "tokens": 1}]
    },
    "nodes": [
      {"id": "H", "type": "task", "priority": 3, "core": 0,
       "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexA"]},
      {"id": "M", "type": "task", "priority": 2, "core": 0,
       "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexB"]},
      {"id": "L", "type": "task", "priority": 1, "core": 0,
       "time": [[4, 4], [20, 20], [1, 1]], "locks": ["mutexA"]},
      {"id": "D", "type": "task", "priority": 99, "core": 0,
       "time": [[0, 0]], "locks": []},
      {"id": "D2", "type": "task", "priority": 99, "core": 0,
       "time": [[0, 0]], "locks": []}
    ],
    "edges": [
      {"source": "D", "target": "M", "label": "5"},
      {"source": "D2", "target": "H", "label": "15"}
    ]
  })";
}

// Nested-lock fixture:
//   J (priority 2) uses [mutexR1, mutexR2] and can hold R1 while requesting R2.
//   K (priority 1) uses mutexR3; H (priority 5) also uses mutexR3, so
//   ceiling(mutexR3)=5.
//   K2 (priority 1) uses mutexR4, so ceiling(mutexR4)=1.
const char* kPcpNestedJson = R"({
  "graph": {"name": "PcpNested"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pcp",
    "shared_locks": ["mutexR1", "mutexR2", "mutexR3", "mutexR4"]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 5, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexR3"]},
    {"id": "J", "type": "task", "priority": 2, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1], [1, 1], [1, 1]],
     "locks": ["mutexR1", "mutexR2"]},
    {"id": "K", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexR3"]},
    {"id": "K2", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexR4"]}
  ],
  "edges": []
})";

// Ceiling tie-break fixture:
//   mutexA is held by A1 (priority 1), mutexB by A2 (priority 3). H (priority
//   5) uses both locks, so ceiling(mutexA) = ceiling(mutexB) = 5.
//   J (priority 4) requests free mutexJ. Its system ceiling is 5, so it is
//   ceiling-blocked; the deterministic instance selects the lexicographically
//   smallest lock name ("mutexA") and its holder A1 for inheritance.
const char* kPcpCeilingTieJson = R"({
  "graph": {"name": "PcpCeilingTie"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pcp",
    "shared_locks": ["mutexA", "mutexB", "mutexJ"]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 5, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1], [1, 1], [1, 1]],
     "locks": ["mutexA", "mutexB"]},
    {"id": "A1", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexA"]},
    {"id": "A2", "type": "task", "priority": 3, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexB"]},
    {"id": "J", "type": "task", "priority": 4, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexJ"]}
  ],
  "edges": []
})";

// Ceiling computation fixture: mutexA has three users (1, 2, 4); H (priority
// 5) is a higher-priority non-user; mutexH has one user (5); mutexUnused has
// none.
const char* kPcpCeilingsJson = R"({
  "graph": {"name": "PcpCeilings"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pcp",
    "shared_locks": ["mutexA", "mutexH", "mutexUnused"]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 5, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexH"]},
    {"id": "A", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexA"]},
    {"id": "B", "type": "task", "priority": 2, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexA"]},
    {"id": "D", "type": "task", "priority": 4, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexA"]}
  ],
  "edges": []
})";

// Admission-threshold fixture: H (priority 3) and L (priority 1) use mutexA;
// M (2), E (3), K (4) and Z (0) each request an independent free lock.
const char* kPcpAdmissionJson = R"({
  "graph": {"name": "PcpAdmission"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pcp",
    "shared_locks": ["mutexA", "mutexB", "mutexC", "mutexD", "mutexE"]
  },
  "nodes": [
    {"id": "H", "type": "task", "priority": 3, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexA"]},
    {"id": "L", "type": "task", "priority": 1, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexA"]},
    {"id": "M", "type": "task", "priority": 2, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexB"]},
    {"id": "E", "type": "task", "priority": 3, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexC"]},
    {"id": "K", "type": "task", "priority": 4, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexD"]},
    {"id": "Z", "type": "task", "priority": 0, "core": 0,
     "time": [[1, 1], [1, 1], [1, 1]], "locks": ["mutexE"]}
  ],
  "edges": []
})";

// Equal-priority branch enumeration under PCP: B1/B2 (priority 5) wait on
// locks held by A1/A2 (priority 1), so both holders inherit 5.
const char* kPcpTieJson = R"({
  "graph": {"name": "PcpTie"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "policy": "pcp",
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

// Two tasks acquire two locks in opposite orders on separate cores. Both have
// priority 1 and both locks have ceiling 1.
//
//   fixed: a reachable interleaving lets A hold mutex1 and B hold mutex2 at
//          the same time; each then requests the other's lock -> deadlock.
//   pcp:   whichever task first acquires a lock raises the system ceiling to
//          1, so the other task's *first* acquire (base 1 <= 1) is
//          inadmissible. Hold-and-wait cannot form, so the cycle is
//          unreachable.
std::string pcp_deadlock_json(const std::string& policy) {
  return R"({
    "graph": {"name": "PcpDeadlock"},
    "configuration": {
      "num_cpus": 2,
      "cores_per_cpu": 1,
      "policy": ")" + policy + R"(",
      "shared_locks": ["mutex1", "mutex2"],
      "start": [{"task": "A", "tokens": 1}, {"task": "B", "tokens": 1}]
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0,
       "time": [[1, 1], [1, 1], [1, 1], [1, 1], [1, 1]],
       "locks": ["mutex1", "mutex2"]},
      {"id": "B", "type": "task", "priority": 1, "core": 1,
       "time": [[1, 1], [1, 1], [1, 1], [1, 1], [1, 1]],
       "locks": ["mutex2", "mutex1"]}
    ],
    "edges": []
  })";
}

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

  // PIP and PCP are analyzed dynamically and use resume-style preemption.
  auto pip_rule = state_class::make_firing_rule(SchedulePolicy::PIP);
  ASSERT_NE(pip_rule, nullptr);
  EXPECT_NE(dynamic_cast<state_class::PipFiringRule*>(pip_rule.get()), nullptr);
  EXPECT_EQ(pip_rule->preemption_mode(), PreemptionMode::RESUME);
  EXPECT_TRUE(pip_rule->admissible(context, 0));
  EXPECT_TRUE(converter::detail::is_resume_policy(SchedulePolicy::PIP));

  auto pcp_rule = state_class::make_firing_rule(SchedulePolicy::PCP);
  ASSERT_NE(pcp_rule, nullptr);
  EXPECT_NE(dynamic_cast<state_class::PcpFiringRule*>(pcp_rule.get()), nullptr);
  EXPECT_EQ(pcp_rule->preemption_mode(), PreemptionMode::RESUME);
  EXPECT_TRUE(pcp_rule->admissible(context, 0));
  EXPECT_TRUE(converter::detail::is_resume_policy(SchedulePolicy::PCP));

  // SRP and the remaining policies fall back to the static rule and
  // non-preemptive handling in this phase.
  for (const SchedulePolicy policy :
       {SchedulePolicy::SRP, SchedulePolicy::EDF, SchedulePolicy::LLF,
        SchedulePolicy::FIFO}) {
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

// ---------------------------------------------------------------------------
// 8. PCP: static lock ceilings + ceiling blocking + inheritance
// ---------------------------------------------------------------------------

namespace {
constexpr size_t kPcpAcquire =
    state_class::detail::TaskLockTopology::kLockAcquireBase;
}  // namespace

TEST(PcpPolicyTest, CeilingBlockingPreventsMediumPriorityPreemption) {
  const petri::PTPN fixed_net = lower(parse_tdg(pcp_ceiling_json("fixed")));
  const petri::PTPN pip_net = lower(parse_tdg(pcp_ceiling_json("pip")));
  const petri::PTPN pcp_net = lower(parse_tdg(pcp_ceiling_json("pcp")));
  ASSERT_EQ(fixed_net.schedule_policy(), SchedulePolicy::FIXED);
  ASSERT_EQ(pip_net.schedule_policy(), SchedulePolicy::PIP);
  ASSERT_EQ(pcp_net.schedule_policy(), SchedulePolicy::PCP);

  const auto fixed_report = analyze(fixed_net);
  const auto pip_report = analyze(pip_net);
  const auto pcp_report = analyze(pcp_net);

  const auto& fixed_h = task_metrics(fixed_report, "H");
  const auto& pip_h = task_metrics(pip_report, "H");
  const auto& pcp_h = task_metrics(pcp_report, "H");
  ASSERT_TRUE(fixed_h.observed);
  ASSERT_TRUE(pip_h.observed);
  ASSERT_TRUE(pcp_h.observed);
  ASSERT_FALSE(fixed_h.wcrt.infinite);
  ASSERT_FALSE(pip_h.wcrt.infinite);
  ASSERT_FALSE(pcp_h.wcrt.infinite);

  // M (priority 2) wants free mutexB while L (priority 1) holds mutexA, whose
  // ceiling is 3 (H). Under fixed/PIP, M acquires and preempts L, delaying
  // H's first job. Under PCP the acquire is inadmissible while the system
  // ceiling is 3, so L keeps running and H finishes earlier.
  EXPECT_GT(fixed_h.wcrt.value, pcp_h.wcrt.value);
  EXPECT_GT(pip_h.wcrt.value, pcp_h.wcrt.value);
  EXPECT_EQ(fixed_h.wcrt.value, 15);
  EXPECT_EQ(pip_h.wcrt.value, 15);
  EXPECT_EQ(pcp_h.wcrt.value, 13);

  // The medium task still executes under PCP, just later.
  EXPECT_TRUE(task_metrics(pcp_report, "M").observed);
}

TEST(PcpPolicyTest, DirectBlockingInheritsAndReverts) {
  // End-to-end: M has no locks, so PCP reduces to direct blocking like PIP.
  const auto fixed_report = analyze(lower(parse_tdg(pip_inversion_json("fixed"))));
  const auto pcp_report = analyze(lower(parse_tdg(pip_inversion_json("pcp"))));
  const auto& fixed_h = task_metrics(fixed_report, "H");
  const auto& pcp_h = task_metrics(pcp_report, "H");
  ASSERT_FALSE(fixed_h.wcrt.infinite);
  ASSERT_FALSE(pcp_h.wcrt.infinite);
  EXPECT_EQ(fixed_h.wcrt.value, 10);
  EXPECT_EQ(pcp_h.wcrt.value, 8);

  // Direct rule: H waits on mutex1 held by L.
  const petri::PTPN net = lower(parse_tdg(kPcpDirectJson));
  const state_class::PcpFiringRule rule(net);
  const size_t l_hold = chain_place(net, "L", kPcpAcquire + 1);
  const size_t h_pre = chain_place(net, "H", kPcpAcquire - 1);
  const size_t h_acquire = chain_transition(net, "H", kPcpAcquire);
  const size_t l_crit = chain_transition(net, "L", kPcpAcquire + 2);
  const size_t l_consume = find_transition(net, "L_consume");
  const size_t lock = net.lock_places().at("mutex1").index();
  ASSERT_NE(l_hold, SIZE_MAX);
  ASSERT_NE(h_pre, SIZE_MAX);
  ASSERT_NE(h_acquire, SIZE_MAX);
  ASSERT_NE(l_crit, SIZE_MAX);
  ASSERT_NE(l_consume, SIZE_MAX);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  state.marking[l_hold] = 1;
  state.marking[h_pre] = 1;
  const PolicyContext context{net, &state};

  // L inherits H's base priority; H keeps its base priority (which is also its
  // effective priority).
  EXPECT_EQ(rule.effective_priority(context, l_crit), 3);
  EXPECT_EQ(rule.effective_priority(context, h_acquire), 3);
  // The held lock is a structural direct block, not an admission decision:
  // admissible() stays true and the enabled-transition filter handles it.
  EXPECT_TRUE(rule.admissible(context, h_acquire));
  // Control transitions keep their static priority.
  EXPECT_EQ(rule.effective_priority(context, l_consume), 0);

  // Reversion: once the lock is free, H is no longer blocked and L returns to
  // its base priority.
  state.marking[lock] = 1;
  EXPECT_EQ(rule.effective_priority(context, l_crit), 1);
  EXPECT_EQ(rule.effective_priority(context, h_acquire), 3);

  // No in-flight jobs at all.
  state.marking[lock] = 0;
  state.marking[l_hold] = 0;
  state.marking[h_pre] = 0;
  EXPECT_EQ(rule.effective_priority(context, l_crit), 1);
}

TEST(PcpPolicyTest, AdmissionThresholdUsesStrictBasePriorityComparison) {
  const petri::PTPN net = lower(parse_tdg(kPcpAdmissionJson));
  const state_class::PcpFiringRule rule(net);
  const auto& locks = net.lock_places();

  const size_t h_hold = chain_place(net, "H", kPcpAcquire + 1);
  const size_t h_consume = find_transition(net, "H_consume");
  const size_t m_acquire = chain_transition(net, "M", kPcpAcquire);
  const size_t e_acquire = chain_transition(net, "E", kPcpAcquire);
  const size_t k_acquire = chain_transition(net, "K", kPcpAcquire);
  const size_t z_acquire = chain_transition(net, "Z", kPcpAcquire);
  ASSERT_NE(h_hold, SIZE_MAX);
  ASSERT_NE(h_consume, SIZE_MAX);
  ASSERT_NE(m_acquire, SIZE_MAX);
  ASSERT_NE(e_acquire, SIZE_MAX);
  ASSERT_NE(k_acquire, SIZE_MAX);
  ASSERT_NE(z_acquire, SIZE_MAX);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  for (const auto& [name, place] : locks) {
    state.marking[place.index()] = 1;
  }
  // H (base 3) holds mutexA; ceiling(mutexA) = 3 (H and L use it).
  state.marking[h_hold] = 1;
  state.marking[locks.at("mutexA").index()] = 0;
  const PolicyContext context{net, &state};

  // base 2 <= system ceiling 3 and base 3 <= 3: both rejected (strict >).
  EXPECT_FALSE(rule.admissible(context, m_acquire));
  EXPECT_FALSE(rule.admissible(context, e_acquire));
  // base 4 > 3: admitted.
  EXPECT_TRUE(rule.admissible(context, k_acquire));
  // base 0 <= 3: rejected.
  EXPECT_FALSE(rule.admissible(context, z_acquire));
  // Non-acquire transitions are never filtered by the ceiling rule.
  EXPECT_TRUE(rule.admissible(context, h_consume));

  // Releasing the only held lock removes the system ceiling; a priority-0
  // task is then admissible (no 0 sentinel).
  state.marking[h_hold] = 0;
  state.marking[locks.at("mutexA").index()] = 1;
  EXPECT_TRUE(rule.admissible(context, z_acquire));
  EXPECT_TRUE(rule.admissible(context, m_acquire));
}

TEST(PcpPolicyTest, LockCeilingsUseMaximumUserBasePriority) {
  const petri::PTPN net = lower(parse_tdg(kPcpCeilingsJson));
  const state_class::PcpFiringRule rule(net);

  // Users A(1), B(2), D(4); the higher-priority non-user H(5) does not count.
  const auto mutex_a = rule.lock_ceiling("mutexA");
  ASSERT_TRUE(mutex_a.has_value());
  EXPECT_EQ(*mutex_a, 4);

  const auto mutex_h = rule.lock_ceiling("mutexH");
  ASSERT_TRUE(mutex_h.has_value());
  EXPECT_EQ(*mutex_h, 5);

  // A lock no task uses has no holder and no ceiling.
  EXPECT_FALSE(rule.lock_ceiling("mutexUnused").has_value());
  EXPECT_FALSE(rule.lock_ceiling("missingLock").has_value());
}

TEST(PcpPolicyTest, NestedLocksAdmitOnlyAboveOtherTasksCeiling) {
  const petri::PTPN net = lower(parse_tdg(kPcpNestedJson));
  const state_class::PcpFiringRule rule(net);
  const auto& locks = net.lock_places();

  // J: two locks, five segments. Its token at seg_2_done (position 8) is
  // inside mutexR1's ownership range and at mutexR2's acquire pre-place.
  const size_t j_holds_r1 = chain_place(net, "J", kPcpAcquire + 3);
  const size_t j_acquire_r2 = chain_transition(net, "J", kPcpAcquire + 4);
  const size_t k_holds_r3 = chain_place(net, "K", kPcpAcquire + 1);
  const size_t k_crit = chain_transition(net, "K", kPcpAcquire + 2);
  const size_t k_after_release = chain_place(net, "K", kPcpAcquire + 3);
  const size_t k2_holds_r4 = chain_place(net, "K2", kPcpAcquire + 1);
  const size_t k2_crit = chain_transition(net, "K2", kPcpAcquire + 2);
  const size_t k_exit = chain_place(net, "K", 10);
  ASSERT_NE(j_holds_r1, SIZE_MAX);
  ASSERT_NE(j_acquire_r2, SIZE_MAX);
  ASSERT_NE(k_holds_r3, SIZE_MAX);
  ASSERT_NE(k_crit, SIZE_MAX);
  ASSERT_NE(k_after_release, SIZE_MAX);
  ASSERT_NE(k2_holds_r4, SIZE_MAX);
  ASSERT_NE(k2_crit, SIZE_MAX);
  ASSERT_NE(k_exit, SIZE_MAX);
  ASSERT_EQ(j_holds_r1, chain_place(net, "J", 8));

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  for (const auto& [name, place] : locks) {
    state.marking[place.index()] = 1;
  }
  state.marking[j_holds_r1] = 1;
  state.marking[locks.at("mutexR1").index()] = 0;  // J holds R1
  state.marking[k_holds_r3] = 1;
  state.marking[locks.at("mutexR3").index()] = 0;  // K holds R3
  const PolicyContext context{net, &state};

  // ceiling(mutexR3) = 5 (H and K use it) > J's base 2, and J's own R1 is
  // excluded from its system ceiling: acquiring the free R2 is inadmissible
  // even though nested acquisition itself is allowed structurally.
  const auto r3_ceiling = rule.lock_ceiling("mutexR3");
  ASSERT_TRUE(r3_ceiling.has_value());
  EXPECT_EQ(*r3_ceiling, 5);
  EXPECT_FALSE(rule.admissible(context, j_acquire_r2));
  // The ceiling wait edge J -> K gives K J's effective priority.
  EXPECT_EQ(rule.effective_priority(context, k_crit), 2);

  // Reversion: K has released R3 (its token is past the release transition and
  // the lock place is full); only J's own lock remains held, so there is no
  // system ceiling and J's acquisition of R2 is admitted. K reverts to base.
  state.marking[k_holds_r3] = 0;
  state.marking[k_after_release] = 1;
  state.marking[locks.at("mutexR3").index()] = 1;
  EXPECT_TRUE(rule.admissible(context, j_acquire_r2));
  EXPECT_EQ(rule.effective_priority(context, k_crit), 1);

  // A different task holds R4, whose ceiling is 1, below J's base priority 2:
  // J is admitted and the holder keeps its base priority.
  state.marking[k_after_release] = 0;
  state.marking[k_exit] = 1;
  state.marking[k2_holds_r4] = 1;
  state.marking[locks.at("mutexR4").index()] = 0;
  EXPECT_TRUE(rule.admissible(context, j_acquire_r2));
  EXPECT_EQ(rule.effective_priority(context, k2_crit), 1);
}

TEST(PcpPolicyTest, CeilingTieBreaksOnLexicographicallySmallestLock) {
  const petri::PTPN net = lower(parse_tdg(kPcpCeilingTieJson));
  const state_class::PcpFiringRule rule(net);
  const auto& locks = net.lock_places();

  const size_t a1_holds_a = chain_place(net, "A1", kPcpAcquire + 1);
  const size_t a2_holds_b = chain_place(net, "A2", kPcpAcquire + 1);
  const size_t j_pre = chain_place(net, "J", kPcpAcquire - 1);
  const size_t a1_crit = chain_transition(net, "A1", kPcpAcquire + 2);
  const size_t a2_crit = chain_transition(net, "A2", kPcpAcquire + 2);
  ASSERT_NE(a1_holds_a, SIZE_MAX);
  ASSERT_NE(a2_holds_b, SIZE_MAX);
  ASSERT_NE(j_pre, SIZE_MAX);
  ASSERT_NE(a1_crit, SIZE_MAX);
  ASSERT_NE(a2_crit, SIZE_MAX);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  for (const auto& [name, place] : locks) {
    state.marking[place.index()] = 1;
  }
  state.marking[a1_holds_a] = 1;
  state.marking[locks.at("mutexA").index()] = 0;  // held by A1, base 1
  state.marking[a2_holds_b] = 1;
  state.marking[locks.at("mutexB").index()] = 0;  // held by A2, base 3
  state.marking[j_pre] = 1;                        // J requests free mutexJ
  const PolicyContext context{net, &state};

  // Both held locks attain the system ceiling 5. The analyzed PCP instance
  // deterministically picks the lexicographically smallest lock name
  // ("mutexA" < "mutexB"), so A1 inherits J's base priority 4 and A2 does not.
  const auto ceiling_a = rule.lock_ceiling("mutexA");
  const auto ceiling_b = rule.lock_ceiling("mutexB");
  ASSERT_TRUE(ceiling_a.has_value());
  ASSERT_TRUE(ceiling_b.has_value());
  EXPECT_EQ(*ceiling_a, 5);
  EXPECT_EQ(*ceiling_b, 5);
  EXPECT_FALSE(rule.admissible(context, chain_transition(net, "J", kPcpAcquire)));
  EXPECT_EQ(rule.effective_priority(context, a1_crit), 4);
  EXPECT_EQ(rule.effective_priority(context, a2_crit), 3);
}

TEST(PcpPolicyTest, OwnLocksDoNotBlockAdmission) {
  const petri::PTPN net = lower(parse_tdg(kPcpNestedJson));
  const state_class::PcpFiringRule rule(net);
  const auto& locks = net.lock_places();

  const size_t j_holds_r1 = chain_place(net, "J", 8);
  const size_t j_acquire_r2 = chain_transition(net, "J", kPcpAcquire + 4);
  ASSERT_NE(j_holds_r1, SIZE_MAX);
  ASSERT_NE(j_acquire_r2, SIZE_MAX);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  for (const auto& [name, place] : locks) {
    state.marking[place.index()] = 1;
  }
  // J holds only its own mutexR1 (ceiling 2). Excluding J's locks leaves an
  // empty system ceiling, so base 2 is admitted despite 2 <= ceiling(R1).
  state.marking[j_holds_r1] = 1;
  state.marking[locks.at("mutexR1").index()] = 0;
  const PolicyContext context{net, &state};
  EXPECT_TRUE(rule.admissible(context, j_acquire_r2));

  // With no locks held by anyone, K2's request is also admitted (base 1).
  state.marking[j_holds_r1] = 0;
  state.marking[locks.at("mutexR1").index()] = 1;
  EXPECT_TRUE(rule.admissible(context, chain_transition(net, "K2", kPcpAcquire)));

  // Priority-0 requesters are admitted too when the system ceiling is empty.
  // kPcpAdmissionJson's Z has base priority 0 and a lock no one else holds.
  const petri::PTPN admission_net = lower(parse_tdg(kPcpAdmissionJson));
  const state_class::PcpFiringRule admission_rule(admission_net);
  state_class::StateClass empty;
  empty.marking.assign(admission_net.num_places(), 0);
  for (const auto& [name, place] : admission_net.lock_places()) {
    empty.marking[place.index()] = 1;
  }
  const PolicyContext empty_context{admission_net, &empty};
  EXPECT_TRUE(admission_rule.admissible(
      empty_context, chain_transition(admission_net, "Z", kPcpAcquire)));
}

TEST(PcpPolicyTest, EqualInheritedPrioritiesEnumerateBothBranches) {
  const petri::PTPN net = lower(parse_tdg(kPcpTieJson));
  const state_class::PcpFiringRule rule(net);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  // A1/A2 hold m1/m2; B1/B2 are blocked acquiring them (direct blocking).
  state.marking[chain_place(net, "A1", kPcpAcquire + 1)] = 1;
  state.marking[chain_place(net, "A2", kPcpAcquire + 1)] = 1;
  state.marking[chain_place(net, "B1", kPcpAcquire - 1)] = 1;
  state.marking[chain_place(net, "B2", kPcpAcquire - 1)] = 1;
  const PolicyContext context{net, &state};

  const size_t a1_crit = chain_transition(net, "A1", kPcpAcquire + 2);
  const size_t a2_crit = chain_transition(net, "A2", kPcpAcquire + 2);
  ASSERT_NE(a1_crit, SIZE_MAX);
  ASSERT_NE(a2_crit, SIZE_MAX);
  EXPECT_EQ(rule.effective_priority(context, a1_crit), 5);
  EXPECT_EQ(rule.effective_priority(context, a2_crit), 5);

  // One core, capacity 1: both inherited-priority candidates are maximal, so
  // both tie resolutions must be enumerated.
  const auto branches =
      Scheduling::priority_branches({a1_crit, a2_crit}, net, rule, context);
  ASSERT_EQ(branches.size(), 2u);
  EXPECT_EQ(branches[0], (state_class::TransitionSet{std::min(a1_crit, a2_crit)}));
  EXPECT_EQ(branches[1], (state_class::TransitionSet{std::max(a1_crit, a2_crit)}));
}

TEST(PcpPolicyTest, CeilingsPreventHoldAndWaitDeadlock) {
  const auto fixed_report = analyze(lower(parse_tdg(pcp_deadlock_json("fixed"))));
  const auto pcp_report = analyze(lower(parse_tdg(pcp_deadlock_json("pcp"))));

  // Without ceilings, the opposite acquisition orders form a hold-and-wait
  // cycle: both tasks can hold one lock and wait for the other.
  EXPECT_FALSE(fixed_report.deadlock_states.empty());
  EXPECT_FALSE(fixed_report.schedulable);

  // PCP admits a task's first acquire only when its base priority exceeds the
  // system ceiling. Both priorities and both ceilings are 1, so the second
  // task cannot acquire while the first holds a lock: the cycle is
  // unreachable.
  EXPECT_TRUE(pcp_report.deadlock_states.empty());
  EXPECT_TRUE(pcp_report.schedulable);
}

TEST(PcpPolicyTest, LoweringUsesResumeResourcesAndRejectsAmbiguousJobs) {
  EXPECT_TRUE(converter::detail::is_resume_policy(SchedulePolicy::PCP));

  const petri::PTPN net = lower(parse_tdg(kPcpDirectJson));
  ASSERT_EQ(net.schedule_policy(), SchedulePolicy::PCP);

  // Resume-style lowering: no structural core place, suspendable execution
  // segments.
  EXPECT_EQ(find_place(net, "core0"), SIZE_MAX);
  const size_t h_exec = find_transition(net, "H_exec_2");
  ASSERT_NE(h_exec, SIZE_MAX);
  EXPECT_TRUE(net.get_transition(petri::TransitionId{h_exec}).suspendable);

  // Both factories return the dynamic PCP rule and behave identically; the
  // policy-only overload indexes lazily from PolicyContext::net.
  auto net_rule = state_class::make_firing_rule(SchedulePolicy::PCP, net);
  auto lazy_rule = state_class::make_firing_rule(SchedulePolicy::PCP);
  ASSERT_NE(dynamic_cast<state_class::PcpFiringRule*>(net_rule.get()), nullptr);
  ASSERT_NE(dynamic_cast<state_class::PcpFiringRule*>(lazy_rule.get()), nullptr);
  EXPECT_EQ(net_rule->preemption_mode(), PreemptionMode::RESUME);
  EXPECT_EQ(lazy_rule->preemption_mode(), PreemptionMode::RESUME);

  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  state.marking[chain_place(net, "L", kPcpAcquire + 1)] = 1;
  state.marking[chain_place(net, "H", kPcpAcquire - 1)] = 1;
  const PolicyContext context{net, &state};
  const size_t l_crit = chain_transition(net, "L", kPcpAcquire + 2);
  EXPECT_EQ(net_rule->effective_priority(context, l_crit), 3);
  EXPECT_EQ(lazy_rule->effective_priority(context, l_crit), 3);

  // Capacity > 1 is rejected for PCP like PIP.
  const std::string capacity_json = R"({
    "graph": {"name": "PcpCapacityTwo"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "pcp",
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
  try {
    lower(parse_tdg(capacity_json));
    FAIL() << "expected lowering to reject task_place_capacity > 1 for pcp";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("task_place_capacity"),
              std::string::npos);
  }

  // More than one initial job per task is rejected for PCP like PIP.
  const std::string start_json = R"({
    "graph": {"name": "PcpStartTokens"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "policy": "pcp",
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
    lower(parse_tdg(start_json));
    FAIL() << "expected lowering to reject multiple initial PCP jobs";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("one initial job"),
              std::string::npos);
  }

  // The default single-job capacity remains accepted.
  EXPECT_NO_THROW(lower(parse_tdg(kPcpDirectJson)));
}

TEST(PcpPolicyTest, AmbiguousChainsFallBackToBasePriority) {
  // Two job tokens in H's chain make the chain position ambiguous. The
  // affected task contributes no inheritance; L stays at its base priority and
  // H keeps its base priority. This is the documented defensive fallback, not
  // a proven upper bound.
  const petri::PTPN net = lower(parse_tdg(kPcpDirectJson));
  const state_class::PcpFiringRule rule(net);
  state_class::StateClass state;
  state.marking.assign(net.num_places(), 0);
  state.marking[chain_place(net, "L", kPcpAcquire + 1)] = 1;
  state.marking[chain_place(net, "H", kPcpAcquire - 1)] = 1;
  state.marking[chain_place(net, "H", 0)] = 1;
  const PolicyContext context{net, &state};
  EXPECT_EQ(rule.effective_priority(
                context, chain_transition(net, "L", kPcpAcquire + 2)),
            1);
  EXPECT_EQ(rule.effective_priority(
                context, chain_transition(net, "H", kPcpAcquire)),
            3);
}


