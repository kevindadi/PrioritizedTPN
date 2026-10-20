#include "dsl/lower.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/diagnostics.h"
#include "model/types.h"

namespace dsl {

namespace {

constexpr const char* kMeasuredMethod = "wall-clock repeated runs (prototype; not certified)";
constexpr const char* kUserEstimateMethod = "user estimate";

std::string default_time_unit(const ast::Program& program) {
  if (program.system.time_unit.empty()) {
    return "ms";
  }
  return program.system.time_unit.front().value == "s" ? "s" : "ms";
}

int to_engine_ticks(const ast::TimeValue& value, const std::string& default_unit) {
  // Validation rejects out-of-range values with E-VAL-221 before lowering is
  // reached; this guard keeps a direct lower() call from silently wrapping.
  if (ast::exceeds_engine_tick_range(value, default_unit)) {
    throw std::invalid_argument("time value exceeds the engine's 32-bit millisecond range");
  }
  return static_cast<int>(value.normalized_ms(default_unit));
}

int positive_or_one(const std::vector<ast::Spanned<std::int64_t>>& values) {
  if (values.empty()) {
    return 1;
  }
  return static_cast<int>(std::max<std::int64_t>(1, values.front().value));
}

tdg::WcetProvenance provenance_of(const ast::TimeDecl& declaration) {
  tdg::WcetProvenance provenance;
  switch (declaration.provenance.kind) {
    case ast::ProvenanceKind::Certified:
      provenance.kind = "certified";
      if (declaration.provenance.by.has_value()) {
        provenance.method = declaration.provenance.by->value;
      }
      if (declaration.provenance.artifact.has_value()) {
        provenance.artifact = declaration.provenance.artifact->value;
      }
      break;
    case ast::ProvenanceKind::Measured:
      provenance.kind = "measured";
      provenance.method = kMeasuredMethod;
      if (!declaration.provenance.measure.artifacts.empty()) {
        provenance.artifact = declaration.provenance.measure.artifacts.front().value;
      }
      break;
    case ast::ProvenanceKind::Estimated:
    default:
      provenance.kind = "estimated";
      provenance.method = declaration.provenance.by.has_value() ? declaration.provenance.by->value
                                                                : kUserEstimateMethod;
      break;
  }
  return provenance;
}

// Resolves the RTSL core spec to the TDG core/allowed_cores pair. `core any`
// expands to the full domain; the caller has already rejected it for `rtsl`
// mode via E-VAL-218, so this path is reached by `allocate` and by direct
// lowering callers.
struct CoreDomain {
  int core = 0;
  std::vector<int> allowed;
};

CoreDomain resolve_core(const ast::TaskDecl& task, int num_cpus) {
  CoreDomain domain;
  domain.core = 0;
  domain.allowed = {0};
  if (task.core.empty()) {
    return domain;
  }

  const ast::CoreSpec& spec = task.core.front();
  switch (spec.kind) {
    case ast::CoreSpec::Kind::Single:
      domain.core = static_cast<int>(spec.single);
      domain.allowed = {domain.core};
      break;
    case ast::CoreSpec::Kind::Set: {
      std::set<int> cores;
      for (const std::int64_t core : spec.set) {
        cores.insert(static_cast<int>(core));
      }
      domain.allowed.assign(cores.begin(), cores.end());
      if (!domain.allowed.empty()) {
        domain.core = domain.allowed.front();
      }
      break;
    }
    case ast::CoreSpec::Kind::Any: {
      const int bounded_cpus = std::max(1, num_cpus);
      domain.allowed.clear();
      domain.allowed.reserve(static_cast<std::size_t>(bounded_cpus));
      for (int core = 0; core < bounded_cpus; ++core) {
        domain.allowed.push_back(core);
      }
      domain.core = 0;
      break;
    }
  }
  return domain;
}

void report(ptpn::Diagnostics& diagnostics, const SourceMap& source_map, const std::string& code,
            const std::string& subject, std::string message, std::string fix) {
  diagnostics.add(code, std::move(message), subject, source_map.location_for(subject),
                  std::move(fix));
}

}  // namespace

std::optional<tdg::TDG> lower(const ast::Program& program, SourceMap& source_map,
                              ptpn::Diagnostics& diagnostics, ValidationOptions::Mode mode) {
  const int num_cpus = positive_or_one(program.system.cpus);
  const int cores_per_cpu = positive_or_one(program.system.cores_per_cpu);
  tdg::TDG result(num_cpus, cores_per_cpu);

  if (!program.system.policy.empty()) {
    const SchedulePolicy policy = parse_schedule_policy(program.system.policy.front().value);
    if (policy != SchedulePolicy::UNKNOWN) {
      result.policy = policy;
    }
  }

  const std::string time_unit = default_time_unit(program);
  bool failed = false;

  // First declaration wins for duplicate ids, matching validate()'s anchor.
  std::map<std::string, const ast::TaskDecl*> tasks;
  for (const ast::TaskDecl& task : program.tasks) {
    tasks.emplace(task.id, &task);
  }

  // Deterministic insertion: task nodes in canonical id order.
  for (const auto& [id, task_pointer] : tasks) {
    const ast::TaskDecl& task = *task_pointer;
    const ast::TaskKind kind = task.effective_kind();

    if (kind == ast::TaskKind::Sporadic || task.time.empty()) {
      // E-UNS-003 / E-VAL-211 are reported by validation; there is no TDG
      // mapping for these shapes.
      failed = true;
      continue;
    }

    const ast::TimeDecl& time = task.time.front();
    if (time.intervals.empty()) {
      if (mode != ValidationOptions::Mode::Check) {
        report(diagnostics, source_map, "E-VAL-213", "task:" + id + ":time",
               "measured time is missing its interval(s)",
               "fill the time interval in the RTSL source first");
      }
      failed = true;
      continue;
    }

    const std::vector<std::string> locks = task.lock_names();
    const std::size_t expected_intervals = 2 * locks.size() + 1;
    if (time.intervals.size() != expected_intervals) {
      report(diagnostics, source_map, "E-VAL-015", "task:" + id,
             "Node " + id + " has " + std::to_string(locks.size()) + " lock(s) but " +
                 std::to_string(time.intervals.size()) + " time interval(s) (expected " +
                 std::to_string(expected_intervals) + ")",
             "provide 2*lock_count+1 time intervals (pre/CS/post per lock)");
      failed = true;
    }

    const CoreDomain core = resolve_core(task, num_cpus);
    if (std::find(core.allowed.begin(), core.allowed.end(), core.core) == core.allowed.end()) {
      report(diagnostics, source_map, "E-VAL-011", "task:" + id,
             "Core " + std::to_string(core.core) + " of node " + id +
                 " is not in its allowed_cores domain",
             "include the task's core in allowed_cores or set core to an allowed one");
      failed = true;
    }

    TaskNode node;
    node.name = id;
    node.core = core.core;
    if (task.priority.empty()) {
      node.priority = 100;
    } else {
      const std::int64_t priority = task.priority.front().value;
      if (priority < std::numeric_limits<int>::min() ||
          priority > std::numeric_limits<int>::max()) {
        // Validation rejects this with E-VAL-221 before lowering is reached.
        throw std::invalid_argument("priority exceeds the engine's 32-bit range");
      }
      node.priority = static_cast<int>(priority);
    }
    node.deadline =
        task.deadline.empty() ? 0 : to_engine_ticks(task.deadline.front().value, time_unit);
    node.time.reserve(time.intervals.size());
    for (const ast::TimeInterval& interval : time.intervals) {
      node.time.emplace_back(to_engine_ticks(interval.min, time_unit),
                             to_engine_ticks(interval.max, time_unit));
    }
    node.lock = locks;
    node.task_type = kind == ast::TaskKind::Interrupt ? TaskType::INTERRUPT : TaskType::NORMAL;

    result.all_task.push_back(node);
    result.tasks_priority.emplace(id, node.priority);
    result.task_allowed_cores[id] = core.allowed;
    result.task_wcet_provenance[id] = provenance_of(time);
    if (!locks.empty()) {
      result.task_locks_map[id] = locks;
      for (const std::string& lock : locks) {
        result.lock_set.insert(lock);
      }
    }
    result.nodes_type.emplace(id, std::move(node));

    if (kind == ast::TaskKind::Periodic) {
      if (task.period.empty()) {
        // Validation reports E-VAL-211; do not create a non-positive release.
        failed = true;
      } else {
        result.periodic_tasks.push_back(
            {id, to_engine_ticks(task.period.front().value, time_unit)});
      }
    }
  }

  // Implicit fork/join nodes and the wire list. Both containers are ordered so
  // the resulting TDG is independent of declaration order.
  std::map<std::string, NodeType> implicit;
  std::set<std::pair<std::string, std::string>> wires;

  for (const ast::EdgeDecl& edge : program.edges) {
    const std::vector<std::string> sources = edge.source_names();
    const std::vector<std::string> targets = edge.target_names();
    source_map.bind(ast::canonical_edge_subject(sources, targets), edge.span);

    if (sources.empty() || targets.empty()) {
      failed = true;
      continue;
    }
    if (sources.size() >= 2 && targets.size() >= 2) {
      // E-VAL-216 rejects multi-to-multi edges before lowering.
      assert(false && "multi-to-multi edges must be rejected by validation");
      failed = true;
      continue;
    }

    if (sources.size() == 1 && targets.size() == 1) {
      wires.emplace(sources.front(), targets.front());
      continue;
    }

    if (sources.size() == 1) {
      const std::string name = ast::canonical_fork_name(sources.front(), targets);
      source_map.bind("fork:" + name, edge.span);
      implicit.emplace(name, NodeType{ForkTask(name)});
      for (const std::string& target : targets) {
        wires.emplace(sources.front(), name);
        wires.emplace(name, target);
      }
      continue;
    }

    const std::string name = ast::canonical_join_name(sources, targets.front());
    source_map.bind("join:" + name, edge.span);
    implicit.emplace(name, NodeType{JoinTask(name)});
    for (const std::string& source : sources) {
      wires.emplace(source, name);
      wires.emplace(name, targets.front());
    }
  }

  if (failed) {
    return std::nullopt;
  }

  for (const auto& [name, node] : implicit) {
    result.nodes_type.emplace(name, node);
  }

  result.tdg_edges.reserve(wires.size());
  for (const auto& [source, target] : wires) {
    result.tdg_edges.push_back({source, target, "", ""});
  }

  std::set<std::string> starts;
  std::set<std::string> ends;
  for (const ast::BindingDecl& binding : program.bindings) {
    if (tasks.count(binding.task.name) == 0) {
      // E-VAL-204 reports unknown references; skip them here.
      continue;
    }
    if (binding.kind == ast::BindingKind::Start) {
      starts.insert(binding.task.name);
    } else {
      ends.insert(binding.task.name);
    }
  }
  for (const std::string& task : starts) {
    result.start_tasks.push_back({task, 1});
  }
  for (const std::string& task : ends) {
    result.end_tasks.push_back(task);
  }

  return result;
}

}  // namespace dsl
