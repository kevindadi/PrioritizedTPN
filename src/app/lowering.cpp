#include "app/lowering.h"

#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>

#include <spdlog/spdlog.h>

#include "common/diagnostics.h"
#include "common/unsupported_class.h"
#include "dsl/source_map.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"

namespace app {

namespace {

// The unsupported-class guards in the TDG lowering throw std::invalid_argument
// with a message naming the offending policy/task/lock/edge. Recover a stable
// structured subject for the diagnostic without changing the lowering layer.
std::optional<std::string> unsupported_subject(const std::string& message,
                                               SchedulePolicy policy) {
  const auto quoted_after = [&](const std::string& marker) -> std::optional<std::string> {
    const auto pos = message.find(marker);
    if (pos == std::string::npos) {
      return std::nullopt;
    }
    const auto start = pos + marker.size();
    const auto end = message.find('\'', start);
    if (end == std::string::npos) {
      return std::nullopt;
    }
    return message.substr(start, end - start);
  };

  if (auto task = quoted_after("task '"); task.has_value()) {
    return "task:" + task.value();
  }
  if (auto task = quoted_after("start binding for '"); task.has_value()) {
    return "task:" + task.value();
  }

  const std::string fork_marker = "fork/join nodes: ";
  if (const auto pos = message.find(fork_marker); pos != std::string::npos) {
    return "node:" + message.substr(pos + fork_marker.size());
  }

  const std::string edge_marker = "releases: edge ";
  if (const auto pos = message.find(edge_marker); pos != std::string::npos) {
    std::string edge = message.substr(pos + edge_marker.size());
    const auto stop = edge.find(" connects");
    if (stop != std::string::npos) {
      edge = edge.substr(0, stop);
    }
    std::string compact;
    for (const char c : edge) {
      if (c != ' ') {
        compact += c;
      }
    }
    return "edge:" + compact;
  }

  return "policy:" + schedule_policy_to_string(policy);
}

}  // namespace

tdg::TDG project_priority_wcrt(const tdg::TDG& tdg, const std::string& target) {
  const auto unsupported = [](const std::string& reason) {
    return ptpn::UnsupportedClassError("--priority-wcrt " + reason, "option:priority-wcrt");
  };
  if ((tdg.policy != SchedulePolicy::FIXED &&
       tdg.policy != SchedulePolicy::FIXED_PRIOR_WITH_RESUME) ||
      tdg.cores_per_cpu != 1 || tdg.task_place_capacity != 1) {
    throw unsupported("requires fixed-priority resume, one slot per core, and unit task capacity");
  }
  if (!tdg.lock_set.empty() ||
      std::any_of(tdg.tdg_edges.begin(), tdg.tdg_edges.end(),
                  [](const TdgEdge& edge) { return !edge.is_self_loop(); })) {
    throw unsupported("requires independent tasks without locks or dependency edges");
  }

  const auto found = tdg.nodes_type.find(target);
  const TaskNode* focus = found == tdg.nodes_type.end() ? nullptr : as_task_node(found->second);
  if (focus == nullptr) {
    throw ptpn::UnsupportedClassError("--priority-wcrt target is not a task: " + target,
                                     "task:" + target);
  }

  // The visible jobs are initially ready and cannot be preempted by new jobs
  // at run time. Hidden jobs have private timers and entirely suspendable CPU
  // service. They can run in otherwise idle time and cannot delay a visible
  // job on the target core. Private releases and dispatches can occur at the
  // same timestamp as a visible event without changing its clock valuation.
  std::set<std::string> hidden;
  for (const auto& [name, node] : tdg.nodes_type) {
    const TaskNode* task = as_task_node(node);
    if (task == nullptr || task->core < 0 || task->core >= tdg.num_cpus ||
        task->time.size() != 1 || !task->lock.empty() || task->time.front().first <= 0 ||
        task->time.front().first > task->time.front().second ||
        task->time.front().second >= petri::INF) {
      throw unsupported("requires real-core tasks with one finite, positive execution segment");
    }
    if (task->core != focus->core || task->priority < focus->priority) {
      hidden.insert(name);
    }
  }

  std::set<std::string> started;
  long long visible_budget = 0;
  for (const auto& binding : tdg.start_tasks) {
    if (binding.tokens > 1 || (binding.tokens == 1 && !started.insert(binding.task).second)) {
      throw unsupported("requires at most one startup job per task");
    }
    if (binding.tokens == 1 && hidden.count(binding.task) == 0) {
      visible_budget += as_task_node(tdg.nodes_type.at(binding.task))->time.front().second;
      if (visible_budget >= petri::INF) {
        throw unsupported("startup execution budget exceeds the clock bound representation");
      }
    }
  }
  if (started.count(target) == 0) {
    throw unsupported("requires one startup job for the target task");
  }
  std::set<std::string> periodic;
  for (const auto& binding : tdg.periodic_tasks) {
    if (hidden.count(binding.task) == 0 || binding.period <= 0 || binding.period >= petri::INF ||
        !periodic.insert(binding.task).second) {
      throw unsupported("allows periodic releases only for hidden tasks with private positive timers");
    }
  }

  tdg::TDG projected = tdg;
  const auto hidden_name = [&](const std::string& name) { return hidden.count(name) != 0; };
  const auto erase_hidden = [&](auto& values, auto name_of) {
    values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
                   return hidden_name(name_of(value));
                 }), values.end());
  };
  erase_hidden(projected.all_task, [](const NodeType& node) {
    return visit_node(node, [](const auto& entry) { return entry.name; });
  });
  erase_hidden(projected.start_tasks, [](const StartBinding& binding) { return binding.task; });
  erase_hidden(projected.end_tasks, [](const std::string& name) { return name; });
  erase_hidden(projected.tdg_edges, [](const TdgEdge& edge) { return edge.source; });
  projected.periodic_tasks.clear();
  for (const auto& name : hidden) {
    projected.nodes_type.erase(name);
    projected.tasks_priority.erase(name);
    projected.task_locks_map.erase(name);
    projected.task_allowed_cores.erase(name);
    projected.task_wcet_provenance.erase(name);
  }
  return projected;
}

// Resolves a lowering-subject key to the RTSL SourceMap entry that anchors it.
// Implicit nodes are bound under `fork:<name>` / `join:<name>` and the policy
// under `policy`, while lowering reports `node:<name>` / `policy:<name>`.
std::optional<ptpn::SourceLocation> source_map_location(const dsl::SourceMap& source_map,
                                                        const std::string& subject) {
  if (const auto location = source_map.location_for(subject); location.has_value()) {
    return location;
  }

  constexpr std::string_view kNodePrefix = "node:";
  if (subject.compare(0, kNodePrefix.size(), kNodePrefix) == 0) {
    const std::string name = subject.substr(kNodePrefix.size());
    if (auto location = source_map.location_for("fork:" + name); location.has_value()) {
      return location;
    }
    if (auto location = source_map.location_for("join:" + name); location.has_value()) {
      return location;
    }
    return std::nullopt;
  }

  constexpr std::string_view kPolicyPrefix = "policy:";
  if (subject.compare(0, kPolicyPrefix.size(), kPolicyPrefix) == 0) {
    return source_map.location_for("policy");
  }
  return std::nullopt;
}

int build_ptpn_from_tdg(const tdg::TDG& tdg, petri::PTPN& ptpn,
                        ptpn::Diagnostics* diagnostics, const dsl::SourceMap* source_map) {
  spdlog::info("\n[PTPN] Converting TDG to PTPN...");
  try {
    converter::TDG2PN::transform(tdg, ptpn);
  } catch (const ptpn::UnsupportedClassError& error) {
    spdlog::error("[PTPN] TDG to PTPN conversion rejected by the supported class: {}",
                  error.what());
    if (diagnostics != nullptr) {
      const std::optional<std::string> subject =
          error.subject().has_value() ? error.subject()
                                      : unsupported_subject(error.what(), tdg.policy);
      std::optional<ptpn::SourceLocation> location;
      if (source_map != nullptr && subject.has_value()) {
        location = source_map_location(*source_map, *subject);
      }
      diagnostics->add("E-UNS-001", error.what(), subject, std::move(location),
                       "simplify the model or choose a supported scheduling policy");
    }
    return 1;
  } catch (const std::exception& error) {
    spdlog::error("[PTPN] TDG to PTPN conversion failed: {}", error.what());
    if (diagnostics != nullptr) {
      diagnostics->add("E-INT-003", error.what(),
                       "policy:" + schedule_policy_to_string(tdg.policy), std::nullopt,
                       "this is an internal lowering failure; report it");
    }
    return 1;
  }
  spdlog::info("[PTPN] PTPN conversion completed");
  spdlog::info("  Places: {}", ptpn.num_places());
  spdlog::info("  Transitions: {}", ptpn.num_transitions());
  spdlog::info("  Policy: {}", schedule_policy_to_string(tdg.policy));
  return 0;
}

}  // namespace app
