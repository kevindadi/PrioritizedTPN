#include "app/describe.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/common.h"
#include "app/input.h"
#include "dsl/ast.h"
#include "dsl/lower.h"
#include "dsl/validate.h"
#include "lower/tdg2pn/policy_priorities.h"
#include "model/tdg.h"
#include "model/types.h"

namespace app {

namespace {

const char* task_kind_name(TaskType type) {
  switch (type) {
    case TaskType::PERIOD:
      return "periodic";
    case TaskType::APERIOD:
      return "aperiodic";
    case TaskType::INTERRUPT:
      return "interrupt";
    case TaskType::NORMAL:
    default:
      return "normal";
  }
}

std::string join(const std::vector<std::string>& values, const std::string& separator) {
  std::string out;
  for (const std::string& value : values) {
    if (!out.empty()) {
      out += separator;
    }
    out += value;
  }
  return out;
}

std::string describe_segments(const TaskNode& task) {  // The lowering rule is 2*lock_count+1 intervals: pre-critical, then for each
  // lock a critical section followed by the next pre/post segment.
  std::ostringstream out;
  const size_t locks = task.lock.size();
  for (size_t i = 0; i < task.time.size(); ++i) {
    if (i) {
      out << " | ";
    }
    if (locks > 0 && i % 2 == 1 && (i - 1) / 2 < locks) {
      out << task.lock[(i - 1) / 2] << " critical section ";
    } else if (locks > 0 && i > 0 && i == 2 * locks) {
      out << "post-critical ";
    } else if (locks > 0 && i % 2 == 0 && i > 0) {
      out << "between locks ";
    } else if (locks > 0) {
      out << "pre-critical ";
    }
    out << "[" << task.time[i].first << ", " << task.time[i].second << "]";
  }
  return out.str();
}

bool is_implicit_node(const std::string& name) {
  return name.rfind("__", 0) == 0;
}

// Inputs and outputs of an implicit fork/join node.
struct ImplicitNodeLinks {
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
};

ImplicitNodeLinks implicit_node_links(const tdg::TDG& tdg, const std::string& node) {
  ImplicitNodeLinks links;
  for (const TdgEdge& edge : tdg.tdg_edges) {
    if (edge.target == node && !is_implicit_node(edge.source)) {
      links.inputs.push_back(edge.source);
    }
    if (edge.source == node && !is_implicit_node(edge.target)) {
      links.outputs.push_back(edge.target);
    }
  }
  return links;
}

// True when the TDG carries a periodic release binding for `name`. The TDG's
// TaskNode type does not record periodicity.
bool periodic_of_task(const tdg::TDG& tdg, const std::string& name) {
  return std::any_of(tdg.periodic_tasks.begin(), tdg.periodic_tasks.end(),
                     [&](const PeriodicBinding& binding) { return binding.task == name; });
}

// Reads the edge statements from the RTSL AST so the description can state the
// token-consuming (alternative / OR) semantics of separate statements, which
// the lowered TDG no longer shows.
struct EdgeStatement {
  std::vector<std::string> sources;
  std::vector<std::string> targets;
  bool fork = false;
  bool join = false;
};

std::vector<EdgeStatement> edge_statements(const dsl::ast::Program& program) {
  std::vector<EdgeStatement> statements;
  for (const dsl::ast::EdgeDecl& edge : program.edges) {
    EdgeStatement statement;
    statement.sources = edge.source_names();
    statement.targets = edge.target_names();
    statement.fork = statement.sources.size() == 1 && statement.targets.size() >= 2;
    statement.join = statement.targets.size() == 1 && statement.sources.size() >= 2;
    if (!statement.sources.empty() && !statement.targets.empty()) {
      statements.push_back(std::move(statement));
    }
  }
  return statements;
}

}  // namespace

std::string build_model_description(const tdg::TDG& tdg, const dsl::ast::Program* program,
                                    const ptpn::Diagnostics& validation,
                                    const DescribeOptions& options) {
  std::ostringstream out;
  const std::string policy =
      options.policy_override.empty() ? schedule_policy_to_string(tdg.policy)
                                      : options.policy_override;
  out << "model: " << tdg.num_cpus << " core(s), cores_per_cpu " << tdg.cores_per_cpu
      << ", policy " << policy << ", tie " << options.tie_mode << "\n";
  out << "  priority semantics: higher value = higher priority; tasks without an explicit "
         "priority use 100\n";
  out << "  deadline semantics: relative to each job's release (a response-time budget)\n";

  // RM/DM recompute priorities during lowering; show the effective ranks.
  std::unordered_map<std::string, int> computed;
  if (tdg.policy == SchedulePolicy::RM || tdg.policy == SchedulePolicy::DM) {
    computed = converter::detail::compute_policy_priorities(tdg);
  }

  out << "\ntasks\n";
  for (const NodeType& node : tdg.all_task) {
    const TaskNode* task = as_task_node(node);
    if (task == nullptr) {
      continue;
    }
    // The TDG only distinguishes interrupt from everything else; the periodic
    // binding and the RTSL declaration carry the rest of the kind.
    std::string kind;
    if (periodic_of_task(tdg, task->name)) {
      kind = "periodic";
    } else if (program != nullptr) {
      for (const dsl::ast::TaskDecl& declaration : program->tasks) {
        if (declaration.id != task->name) {
          continue;
        }
        switch (declaration.effective_kind()) {
          case dsl::ast::TaskKind::Aperiodic:
            kind = "aperiodic (one-shot; same semantics as normal)";
            break;
          case dsl::ast::TaskKind::Interrupt:
            kind = "interrupt (lowered as a one-shot task; no interrupt semantics)";
            break;
          case dsl::ast::TaskKind::Periodic:
            kind = "periodic";
            break;
          case dsl::ast::TaskKind::Sporadic:
            kind = "sporadic (rejected)";
            break;
          case dsl::ast::TaskKind::Normal:
          default:
            kind = "normal (one-shot)";
            break;
        }
        break;
      }
    }
    if (kind.empty()) {
      kind = task_kind_name(task->task_type);
    }
    out << "  " << task->name << ": " << kind << ", core " << task->core;
    const auto computed_it = computed.find(task->name);
    if (computed_it != computed.end()) {
      out << ", priority " << task->priority << " (the " << policy << " policy computes "
          << computed_it->second << " during lowering)";
    } else {
      out << ", priority " << task->priority;
    }
    out << "\n";
    if (task->deadline > 0) {
      out << "    deadline " << task->deadline << " (relative to release)\n";
    }
    out << "    execution: " << describe_segments(*task) << " (" << task->time.size()
        << (task->time.size() == 1 ? " segment" : " segments") << ")\n";
  }

  const auto periodic_of = [&](const std::string& name) -> const PeriodicBinding* {
    const auto it = std::find_if(tdg.periodic_tasks.begin(), tdg.periodic_tasks.end(),
                                 [&](const PeriodicBinding& binding) {
                                   return binding.task == name;
                                 });
    return it == tdg.periodic_tasks.end() ? nullptr : &*it;
  };
  const auto is_started = [&](const std::string& name) {
    return std::any_of(tdg.start_tasks.begin(), tdg.start_tasks.end(),
                       [&](const StartBinding& binding) { return binding.task == name; });
  };

  out << "\nreleases\n";
  for (const NodeType& node : tdg.all_task) {
    const TaskNode* task = as_task_node(node);
    if (task == nullptr) {
      continue;
    }
    std::vector<std::string> sources;
    if (is_started(task->name)) {
      sources.push_back("t=0 (start binding)");
    }
    if (const PeriodicBinding* periodic = periodic_of(task->name); periodic != nullptr) {
      sources.push_back("every " + std::to_string(periodic->period) +
                        " engine ticks (first release at " + std::to_string(periodic->period) +
                        (is_started(task->name) ? "; the start binding adds t=0)" : ")"));
    }
    // Direct task-to-task dependencies; implicit fork/join hops are described
    // in the dependencies section.
    for (const TdgEdge& edge : tdg.tdg_edges) {
      if (edge.target != task->name || edge.source == task->name) {
        continue;
      }
      if (is_implicit_node(edge.source)) {
        const ImplicitNodeLinks links = implicit_node_links(tdg, edge.source);
        const bool fork = edge.source.rfind("__fork", 0) == 0;
        sources.push_back(std::string(fork ? "when " : "when all of ") + join(links.inputs, ", ") +
                          (fork ? " completes (through the fork " : " complete (through the join ") +
                          edge.source + ")");
      } else {
        sources.push_back("when " + edge.source + " completes");
      }
    }
    if (sources.empty()) {
      out << "  " << task->name << ": never released (no start binding, no period, no edge)\n";
      continue;
    }
    out << "  " << task->name << ": " << join(sources, "; ") << "\n";
  }

  out << "\ndependencies\n";
  std::vector<std::string> plain;
  std::set<std::string> reported_implicit;
  for (const TdgEdge& edge : tdg.tdg_edges) {
    if (is_implicit_node(edge.source) || is_implicit_node(edge.target)) {
      // One line per implicit fork/join node, deduplicated.
      const std::string node = is_implicit_node(edge.source) ? edge.source : edge.target;
      if (!reported_implicit.insert(node).second) {
        continue;
      }
      std::vector<std::string> inputs;
      std::vector<std::string> outputs;
      for (const TdgEdge& other : tdg.tdg_edges) {
        if (other.target == node && !is_implicit_node(other.source)) {
          inputs.push_back(other.source);
        }
        if (other.source == node && !is_implicit_node(other.target)) {
          outputs.push_back(other.target);
        }
      }      if (node.rfind("__fork", 0) == 0) {
        out << "  {" << join(inputs, ", ") << "} -> {" << join(outputs, ", ")
            << "}: AND fork (" << node << "): every target is released when the source "
               "completes\n";
      } else {
        out << "  {" << join(inputs, ", ") << "} -> {" << join(outputs, ", ")
            << "}: AND join (" << node << "): the target waits for every source\n";
      }
      continue;
    }
    if (edge.source == edge.target) {
      out << "  " << edge.source << " -> " << edge.target
          << ": self-loop annotation (no release)\n";
      continue;
    }
    plain.push_back("  " + edge.source + " -> " + edge.target +
                    (edge.label.empty() ? std::string() : " (label " + edge.label + ")"));
  }
  for (const std::string& line : plain) {
    out << line << "\n";
  }
  if (program != nullptr) {
    // Separate statements that share a source or target are the token-consuming
    // (alternative / OR) form, not an AND fork/join. The plain edges themselves
    // are already listed above from the lowered TDG.
    std::map<std::string, int> outgoing;
    std::map<std::string, int> incoming;
    for (const EdgeStatement& statement : edge_statements(*program)) {
      if (statement.sources.size() == 1 && statement.targets.size() == 1) {
        ++outgoing[statement.sources.front()];
        ++incoming[statement.targets.front()];
      }
    }
    for (const auto& [source, count] : outgoing) {
      if (count > 1) {
        out << "  note: " << source << " has " << count
            << " separate outgoing edge statements: its exit token is consumed by whichever "
               "bridge fires first, so the targets are alternatives (only one is released). "
               "Use one '"
            << source << " -> {t1, t2}' statement for an AND fork.\n";
      }
    }
    for (const auto& [target, count] : incoming) {
      if (count > 1) {
        out << "  note: " << target << " has " << count
            << " separate incoming edge statements: it is released by whichever source "
               "completes first and can run once per completion. Use one '{s1, s2} -> "
            << target << "}' statement for an AND join.\n";
      }
    }
  }

  if (!tdg.lock_set.empty()) {
    out << "\nlocks\n";
    for (const std::string& lock : tdg.lock_set) {
      std::vector<std::string> users;
      for (const auto& [task, locks] : tdg.task_locks_map) {
        if (std::find(locks.begin(), locks.end(), lock) != locks.end()) {
          users.push_back(task);
        }
      }
      std::sort(users.begin(), users.end());
      out << "  " << lock << ": used by " << join(users, ", ")
          << " (critical sections are the odd-indexed execution segments)\n";
    }
  }

  std::vector<const ptpn::Diagnostic*> notes;
  for (const ptpn::Diagnostic& diagnostic : validation.entries()) {
    if (diagnostic.severity == "hint" || diagnostic.severity == "warning") {
      notes.push_back(&diagnostic);
    }
  }
  if (!notes.empty()) {
    out << "\nnotes (from validation; they do not block the analysis)\n";
    for (const ptpn::Diagnostic* note : notes) {
      out << "  " << note->code << " " << (note->subject.has_value() ? *note->subject : "") << ": "
          << note->message << "\n";
    }
  }

  if (program != nullptr) {
    const std::size_t edges = program->edges.size();
    out << "\nsummary: " << tdg.all_task.size() << " task(s), " << edges << " edge statement(s), "
        << tdg.lock_set.size() << " lock(s)\n";
  }
  return out.str();
}

int run_rtsl_describe(const DescribeOptions& options, ptpn::Diagnostics* diagnostics) {
  ptpn::Diagnostics local;
  ptpn::Diagnostics& collector = diagnostics != nullptr ? *diagnostics : local;

  std::optional<std::string> source = read_rtsl_source(options.input_file);
  if (!source.has_value()) {
    if (diagnostics != nullptr) {
      add_rtsl_open_error(options.input_file, *diagnostics);
      return diagnostics->exit_code();
    }
    std::cerr << "ERROR: failed to open RTSL file: " << options.input_file << '\n';
    return 1;
  }

  RtslDocument document = parse_and_validate_rtsl(
      std::move(source.value()), options.input_file, dsl::ValidationOptions::Mode::Check, collector);
  if (!document.valid) {
    if (diagnostics == nullptr) {
      print_legacy_diagnostics(collector);
    }
    return diagnostics == nullptr ? 1 : collector.exit_code();
  }

  // A pending `measured by llm` task cannot be lowered; report the pending list
  // instead of a description so the caller fills the timing first.
  const nlohmann::json pending = dsl::collect_pending_measurements(document.program,
                                                                   document.source_map);
  if (!pending.empty()) {
    std::ostringstream out;
    out << "model: " << options.input_file << "\n";
    out << "pending measurements: fill each task's `time` attribute before describing the "
           "semantics\n";
    for (const nlohmann::json& entry : pending) {
      out << "  " << entry.at("task").get<std::string>() << ": needs "
          << entry.at("expected_segments").get<int>() << " interval(s)\n";
    }
    const std::string text = out.str();
    if (diagnostics != nullptr) {
      collector.set_result(nlohmann::json{{"description", text}});
      if (diagnostics->exit_code() != 0) {
        return diagnostics->exit_code();
      }
    }
    std::cout << text;
    return 0;
  }

  std::optional<tdg::TDG> lowered = dsl::lower(document.program, document.source_map, collector,
                                               dsl::ValidationOptions::Mode::Check);
  if (!lowered.has_value()) {
    if (diagnostics == nullptr) {
      print_legacy_diagnostics(collector);
    }
    return diagnostics == nullptr ? 1 : collector.exit_code();
  }
  tdg::TDG tdg = std::move(lowered.value());
  if (!options.policy_override.empty()) {
    const SchedulePolicy policy = parse_schedule_policy(options.policy_override);
    if (policy != SchedulePolicy::UNKNOWN) {
      tdg.policy = policy;
    }
  }

  const std::string text =
      build_model_description(tdg, &document.program, collector, options);
  if (diagnostics != nullptr) {
    collector.set_result(nlohmann::json{{"description", text}});
    if (collector.exit_code() != 0) {
      return collector.exit_code();
    }
  }
  std::cout << text;
  return 0;
}

}  // namespace app
