#include "lower/tdg2pn/tdg2pn.h"

#include <exception>
#include <map>
#include <set>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

#include "lower/tdg2pn/binding_lowering.h"
#include "lower/tdg2pn/edge_lowering.h"
#include "lower/tdg2pn/policy_priorities.h"
#include "lower/tdg2pn/preemption.h"
#include "lower/tdg2pn/resource_lowering.h"
#include "lower/tdg2pn/task_metadata.h"
#include "lower/tdg2pn/vertex_lowering.h"

#include "common/unsupported_class.h"

namespace converter {

namespace {

// The static supported class of a policy, expressed as features instead of one
// near-duplicate validator per policy. `validate_supported_class` enforces the
// shared part plus the per-policy additions, so a new policy is one table entry
// and a policy change cannot silently update only one of the copies.
struct SupportedClassFeatures {
  // EDF, FIFO, LLF: independent periodic/one-shot tasks, one in-flight job per
  // task, one running job per real core, no locks, no fork/join nodes and no
  // dependency-driven releases.
  bool independent_tasks = false;
  // EDF, LLF: every task needs a positive absolute deadline or period; their
  // ordering is undefined otherwise.
  bool requires_deadline_or_period = false;
  // LLF: chi is only carried exactly across deterministic execution segments.
  bool requires_deterministic_segments = false;
  // PIP, PCP, SRP: lock ownership is inferred from the position of the single
  // job token in the task chain, so the chain must hold at most one job.
  bool single_job_chain = false;
};

// The policies whose supported class is enforced here. A policy that is not
// listed is lowered without a static supported-class check.
const SupportedClassFeatures* supported_class_features(SchedulePolicy policy) {
  static const std::map<SchedulePolicy, SupportedClassFeatures> table = [] {
    SupportedClassFeatures edf;
    edf.independent_tasks = true;
    edf.requires_deadline_or_period = true;

    SupportedClassFeatures fifo;
    fifo.independent_tasks = true;

    SupportedClassFeatures llf = edf;
    llf.requires_deterministic_segments = true;

    SupportedClassFeatures locks;
    locks.single_job_chain = true;

    return std::map<SchedulePolicy, SupportedClassFeatures>{
        {SchedulePolicy::EDF, edf}, {SchedulePolicy::FIFO, fifo},
        {SchedulePolicy::LLF, llf}, {SchedulePolicy::PIP, locks},
        {SchedulePolicy::PCP, locks}, {SchedulePolicy::SRP, locks}};
  }();
  const auto it = table.find(policy);
  return it == table.end() ? nullptr : &it->second;
}

// EDF, FIFO and LLF are only analyzed for a deliberately restricted,
// statically partitioned, independent-task class. Reject everything else here
// (with a lowering-time message) instead of letting the state-class rule
// approximate an unsupported model.
void validate_independent_tasks(const tdg::TDG& tdg, const std::string& name,
                                const SupportedClassFeatures& features) {
  if (tdg.num_cpus < 1) {
    throw ptpn::UnsupportedClassError(name + " requires num_cpus >= 1; configuration has num_cpus = " +
                                std::to_string(tdg.num_cpus));
  }
  if (tdg.cores_per_cpu != 1) {
    throw ptpn::UnsupportedClassError(
        name + " requires cores_per_cpu == 1 (one running job per real core), got " +
        std::to_string(tdg.cores_per_cpu));
  }
  if (tdg.task_place_capacity != 1) {
    throw ptpn::UnsupportedClassError(
        name + " requires task_place_capacity == 1 (one in-flight job per task), got " +
        std::to_string(tdg.task_place_capacity));
  }

  std::map<std::string, int> period_of;
  for (const auto& binding : tdg.periodic_tasks) {
    if (binding.period <= 0) {
      throw ptpn::UnsupportedClassError(name + " requires a positive period for periodic task '" +
                                  binding.task + "', got " + std::to_string(binding.period));
    }
    period_of[binding.task] = binding.period;
  }

  std::set<std::string> started;
  for (const auto& start : tdg.start_tasks) {
    if (start.tokens < 0 || start.tokens > 1) {
      throw ptpn::UnsupportedClassError(
          name + " requires at most one initial job per task; start binding for '" + start.task +
          "' requests " + std::to_string(start.tokens) + " tokens");
    }
    if (start.tokens == 1) {
      started.insert(start.task);
    }
  }

  const auto check_task = [&](const std::string& task_name, const TaskNode& task) {
    if (task.core < 0 || task.core >= tdg.num_cpus) {
      throw ptpn::UnsupportedClassError(name + " task '" + task_name + "' is bound to core " +
                                  std::to_string(task.core) + " outside [0, " +
                                  std::to_string(tdg.num_cpus) + ")");
    }
    if (!task.lock.empty()) {
      throw ptpn::UnsupportedClassError(name + " supported class does not include locks; task '" +
                                  task_name + "' declares locks");
    }
    const auto period_it = period_of.find(task_name);
    const int period = period_it == period_of.end() ? 0 : period_it->second;
    if (features.requires_deadline_or_period && task.deadline <= 0 && period <= 0) {
      throw ptpn::UnsupportedClassError(name +
                                  " requires a positive absolute deadline or period for "
                                  "task '" +
                                  task_name + "'");
    }
    if (period <= 0 && started.find(task_name) == started.end()) {
      throw ptpn::UnsupportedClassError(
          name + " task '" + task_name +
          "' is neither periodic (released by _fire) nor started at time 0");
    }
    if (!features.requires_deterministic_segments) {
      return;
    }
    if (task.time.empty()) {
      throw ptpn::UnsupportedClassError(name + " task '" + task_name + "' has no execution segments");
    }
    for (const auto& segment : task.time) {
      if (segment.first != segment.second) {
        throw ptpn::UnsupportedClassError(name +
                                    " supports only deterministic execution segments "
                                    "(earliest == latest); task '" +
                                    task_name + "' has an interval [" +
                                    std::to_string(segment.first) + ", " +
                                    std::to_string(segment.second) + "]");
      }
      if (segment.first < 0) {
        throw ptpn::UnsupportedClassError(name + " requires non-negative execution times; task '" +
                                    task_name + "' has segment [" + std::to_string(segment.first) +
                                    ", " + std::to_string(segment.second) + "]");
      }
    }
  };

  for (const auto& node : tdg.all_task) {
    if (const auto* task = as_task_node(node); task != nullptr) {
      check_task(task->name, *task);
    }
  }
  for (const auto& [task_name, node] : tdg.nodes_type) {
    if (const auto* task = as_task_node(node); task != nullptr) {
      check_task(task_name, *task);
    }
  }

  for (const auto& [task_name, locks] : tdg.task_locks_map) {
    if (!locks.empty()) {
      throw ptpn::UnsupportedClassError(name + " supported class does not include locks; task '" +
                                  task_name + "' declares locks");
    }
  }

  for (const auto& [node_name, node] : tdg.nodes_type) {
    if (is_fork_or_join(node)) {
      throw ptpn::UnsupportedClassError(
          name + " supported class does not include fork/join nodes: " + node_name);
    }
  }

  // A normal task-to-task edge lowers to a dependency-driven release bridge.
  // Self-loops are annotations and dashed edges are handled by periodic
  // bindings, so neither is a release edge.
  for (const auto& edge : tdg.tdg_edges) {
    if (edge.is_self_loop() || edge.is_dashed()) {
      continue;
    }
    const auto source_it = tdg.nodes_type.find(edge.source);
    const auto target_it = tdg.nodes_type.find(edge.target);
    if (source_it == tdg.nodes_type.end() || target_it == tdg.nodes_type.end()) {
      continue;
    }
    if (as_task_node(source_it->second) != nullptr && as_task_node(target_it->second) != nullptr) {
      throw ptpn::UnsupportedClassError(
          name + " supported class does not include dependency-driven releases: edge " +
          edge.source + " -> " + edge.target +
          " connects two tasks (independent periodic/one-shot tasks only)");
    }
  }
}

void validate_supported_class(const tdg::TDG& tdg, SchedulePolicy policy,
                              const SupportedClassFeatures& features) {
  const std::string name = schedule_policy_to_string(policy);
  if (features.independent_tasks) {
    validate_independent_tasks(tdg, name, features);
  }
  if (features.single_job_chain) {
    if (tdg.task_place_capacity != 1) {
      throw ptpn::UnsupportedClassError(
          name + " requires task_place_capacity == 1 (one in-flight job per task), got " +
          std::to_string(tdg.task_place_capacity));
    }
    for (const auto& start : tdg.start_tasks) {
      if (start.tokens < 0 || start.tokens > 1) {
        throw ptpn::UnsupportedClassError(
            name + " requires at most one initial job per task; start binding for '" + start.task +
            "' requests " + std::to_string(start.tokens) + " tokens");
      }
    }
  }
}

}  // namespace

void TDG2PN::transform(const tdg::TDG& tdg, petri::PTPN& ptpn) {
  try {
    spdlog::info("[TDG2PN] Starting TDG to PTPN transformation");

    // Lower a local copy so RM/DM can rewrite task priorities (the public
    // signature stays const and the caller's TDG is never mutated).
    tdg::TDG configured = tdg;
    detail::apply_policy_priorities(configured);

    // One table entry per policy: EDF/FIFO/LLF share the independent-task
    // class (LLF adds deterministic segments), PIP/PCP/SRP require a single
    // job token per chain because they infer lock ownership from its position.
    if (const SupportedClassFeatures* supported =
            supported_class_features(configured.policy);
        supported != nullptr) {
      validate_supported_class(configured, configured.policy, *supported);
    }

    ptpn.clear_lowering_metadata();
    ptpn.set_schedule_policy(configured.policy);

    spdlog::info("[TDG2PN] Transforming vertices");
    detail::lower_vertices(ptpn, configured);

    spdlog::info("[TDG2PN] Transforming edges");
    detail::lower_edges(ptpn, configured);

    detail::add_start_bindings(ptpn, configured);
    detail::add_periodic_release_bindings(ptpn, configured);
    detail::add_end_consumers(ptpn, configured);

    detail::make_preemption_strategy(configured.policy)->apply(ptpn, configured);

    spdlog::info("[TDG2PN] Adding resources and bindings");
    detail::add_resources_and_bindings(ptpn, configured);

    detail::populate_task_info(ptpn, configured);

    spdlog::info("[TDG2PN] TDG transformation completed: {} places, {} transitions",
                 ptpn.num_places(), ptpn.num_transitions());

    if (!ptpn.verify_structure()) {
      spdlog::warn("[TDG2PN] Structure verification failed, continuing anyway");
    }
  } catch (const std::exception& e) {
    spdlog::error("[TDG2PN] Failed to transform TDG to PTPN: {}", e.what());
    throw;
  }
}

}  // namespace converter
