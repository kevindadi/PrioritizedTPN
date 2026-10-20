#include "dsl/validate.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "model/types.h"

namespace dsl {

namespace {

constexpr const char* kLockFix = "rename the lock to start with 'mutex' or 'spin'";
constexpr const char* kUnitFix = "use 'ms' or 's'";

std::optional<ptpn::SourceLocation> location_of(const Span& span) {
  if (!span.valid()) {
    return std::nullopt;
  }
  ptpn::SourceLocation location;
  location.file = span.file;
  location.line = span.line;
  location.column = span.column;
  location.span = static_cast<int>(span.length());
  return location;
}

void report(ptpn::Diagnostics& diagnostics, std::string code, std::string subject, const Span& span,
            std::string message, std::string fix, std::string severity = std::string()) {
  ptpn::Diagnostic diagnostic;
  diagnostic.code = std::move(code);
  diagnostic.subject = std::move(subject);
  diagnostic.message = std::move(message);
  diagnostic.fix = std::move(fix);
  diagnostic.location = location_of(span);
  if (!severity.empty()) {
    diagnostic.severity = std::move(severity);
  }
  diagnostics.add(std::move(diagnostic));
}

template <typename T>
void report_duplicate_attributes(ptpn::Diagnostics& diagnostics, const std::vector<T>& attributes,
                                 const std::string& subject, const std::string& label) {
  for (std::size_t index = 1; index < attributes.size(); ++index) {
    if (diagnostics.is_tainted(subject)) {
      return;
    }
    report(diagnostics, "E-VAL-219", subject, attributes[index].span,
           "attribute '" + label + "' is declared more than once",
           "remove the duplicate declaration");
  }
}

bool has_lock_prefix(const std::string& name) {
  const auto starts_with = [&name](const char* prefix) {
    const std::string text(prefix);
    return name.size() >= text.size() && name.compare(0, text.size(), text) == 0;
  };
  return starts_with("mutex") || starts_with("spin");
}

std::string describe_position(const Span& span) {
  return "line " + std::to_string(span.line) + ", column " + std::to_string(span.column);
}

struct SystemInfo {
  bool cpus_known = false;
  std::int64_t cpus = 0;
  std::string time_unit = "ms";
};

class Validator {
 public:
  Validator(const ast::Program& program, SourceMap& source_map, ptpn::Diagnostics& diagnostics,
            const ValidationOptions& options)
      : program_(program), source_map_(source_map), diagnostics_(diagnostics), options_(options) {}

  void run() {
    validate_system();
    validate_tasks();
    validate_edges();
    validate_bindings();
    validate_schedule_hints();
  }

 private:
  void validate_system();
  void validate_tasks();
  void validate_task(const ast::TaskDecl& task);
  void validate_time_decl(const ast::TaskDecl& task, const ast::TimeDecl& declaration);
  void validate_time_value(const ast::TimeValue& value, const std::string& subject);
  void validate_core(const ast::TaskDecl& task);
  void validate_locks(const ast::TaskDecl& task);
  void validate_edges();
  void validate_bindings();
  void validate_schedule_hints();

  const ast::Program& program_;
  SourceMap& source_map_;
  ptpn::Diagnostics& diagnostics_;
  ValidationOptions options_;
  SystemInfo system_;
  std::set<std::string> task_ids_;
  std::set<std::string> reported_locks_;
};

void Validator::validate_system() {
  const ast::SystemBlock& system = program_.system;

  report_duplicate_attributes(diagnostics_, system.cpus, "system:cpus", "cpus");
  report_duplicate_attributes(diagnostics_, system.cores_per_cpu, "system:cores_per_cpu",
                              "cores_per_cpu");
  report_duplicate_attributes(diagnostics_, system.time_unit, "system:time_unit", "time_unit");
  report_duplicate_attributes(diagnostics_, system.policy, "policy", "policy");

  if (!diagnostics_.is_tainted("system:cpus")) {
    if (system.cpus.empty()) {
      report(diagnostics_, "E-VAL-217", "system:cpus", system.span,
             "the system block is missing the required 'cpus' attribute",
             "add 'cpus N' with N >= 1");
    } else {
      system_.cpus = system.cpus.front().value;
      if (system_.cpus < 1) {
        report(diagnostics_, "E-VAL-217", "system:cpus", system.cpus.front().span,
               "cpus must be at least 1", "set cpus to a positive core count");
      } else if (system_.cpus > std::numeric_limits<int>::max()) {
        report(diagnostics_, "E-VAL-221", "system:cpus", system.cpus.front().span,
               "cpus does not fit the engine's 32-bit core range",
               "use at most 2147483647 cores");
      } else {
        system_.cpus_known = true;
      }
    }
  }

  if (!diagnostics_.is_tainted("system:cores_per_cpu") && !system.cores_per_cpu.empty()) {
    if (system.cores_per_cpu.front().value < 1) {
      report(diagnostics_, "E-VAL-217", "system:cores_per_cpu", system.cores_per_cpu.front().span,
             "cores_per_cpu must be at least 1", "set cores_per_cpu to a positive value");
    } else if (system.cores_per_cpu.front().value > std::numeric_limits<int>::max()) {
      report(diagnostics_, "E-VAL-221", "system:cores_per_cpu",
             system.cores_per_cpu.front().span,
             "cores_per_cpu does not fit the engine's 32-bit core range",
             "use at most 2147483647 cores per CPU");
    }
  }

  if (!diagnostics_.is_tainted("system:time_unit") && !system.time_unit.empty()) {
    const ast::Spanned<std::string>& unit = system.time_unit.front();
    if (unit.value == "us") {
      report(diagnostics_, "E-UNS-004", "system:time_unit", unit.span,
             "sub-millisecond time unit 'us' is not supported",
             "use ms and adjust the model precision");
    } else if (unit.value != "ms" && unit.value != "s") {
      report(diagnostics_, "E-SYN-101", "system:time_unit", unit.span,
             "unknown time unit '" + unit.value + "' (expected 'ms' or 's')", kUnitFix);
    } else {
      system_.time_unit = unit.value;
    }
  }

  if (!diagnostics_.is_tainted("policy") && !system.policy.empty()) {
    const ast::Spanned<std::string>& policy = system.policy.front();
    if (parse_schedule_policy(policy.value) == SchedulePolicy::UNKNOWN) {
      report(diagnostics_, "E-VAL-215", "policy", policy.span,
             "unknown scheduling policy '" + policy.value + "'",
             "use one of: fixed, fixed_prior_with_resume, fixed_prior_with_restart, rm, dm, "
             "edf, fifo, llf, pip, pcp, or srp");
    }
  }
}

void Validator::validate_tasks() {
  std::map<std::string, const ast::TaskDecl*> first_seen;
  for (const ast::TaskDecl& task : program_.tasks) {
    const std::string subject = "task:" + task.id;
    task_ids_.insert(task.id);
    const auto [iterator, inserted] = first_seen.emplace(task.id, &task);
    if (diagnostics_.is_tainted(subject)) {
      continue;
    }
    if (!inserted) {
      report(diagnostics_, "E-VAL-201", subject, task.id_span,
             "duplicate task id '" + task.id + "' (first declared at " +
                 describe_position(iterator->second->id_span) + ")",
             "rename one of the duplicate tasks");
    }
    validate_task(task);
  }
}

void Validator::validate_time_value(const ast::TimeValue& value, const std::string& subject) {
  if (value.unit == "us") {
    report(diagnostics_, "E-UNS-004", subject, value.span,
           "sub-millisecond time unit 'us' is not supported",
           "use ms and adjust the model precision");
  } else if (!value.unit.empty() && value.unit != "ms" && value.unit != "s") {
    report(diagnostics_, "E-SYN-101", subject, value.span,
           "unknown time unit suffix '" + value.unit + "' (expected 'ms' or 's')", kUnitFix);
  }

  // The engine stores time as signed 32-bit millisecond ticks. Without this
  // check the lowering cast silently wraps (for example 3000000s becomes a
  // negative interval) and only surfaces later as an internal error.
  if (ast::exceeds_engine_tick_range(value, system_.time_unit)) {
    report(diagnostics_, "E-VAL-221", subject, value.span,
           "time value does not fit the engine's 32-bit millisecond range",
           "reduce the value to at most 2147483646 ms");
  }
}

void Validator::validate_task(const ast::TaskDecl& task) {
  const std::string base = "task:" + task.id;

  if (task.id.find("__") != std::string::npos) {
    report(diagnostics_, "E-VAL-202", base, task.id_span,
           "task id '" + task.id + "' contains '__', which is reserved for implicit nodes",
           "rename the task without '__'");
  }

  report_duplicate_attributes(diagnostics_, task.period, base + ":period", "period");
  report_duplicate_attributes(diagnostics_, task.deadline, base + ":deadline", "deadline");
  report_duplicate_attributes(diagnostics_, task.mit, base + ":mit", "mit");
  report_duplicate_attributes(diagnostics_, task.priority, base + ":priority", "priority");
  report_duplicate_attributes(diagnostics_, task.core, base + ":core", "core");
  report_duplicate_attributes(diagnostics_, task.locks, base + ":locks", "locks");
  report_duplicate_attributes(diagnostics_, task.time, base + ":time", "time");

  const ast::TaskKind kind = task.effective_kind();

  // Priorities are compared as signed 32-bit ints by the engine; a larger
  // literal would silently wrap during lowering.
  for (const ast::Spanned<std::int64_t>& priority : task.priority) {
    if (priority.value < std::numeric_limits<int>::min() ||
        priority.value > std::numeric_limits<int>::max()) {
      report(diagnostics_, "E-VAL-221", base + ":priority", priority.span,
             "priority does not fit the engine's 32-bit priority range",
             "use a priority between -2147483648 and 2147483647");
    }
  }

  if (!task.period.empty() && kind != ast::TaskKind::Periodic) {
    report(diagnostics_, "E-VAL-205", base + ":period", task.period.front().span,
           "attribute 'period' is only valid on periodic tasks",
           "remove 'period' or declare the task as 'periodic'");
  }
  if (!task.mit.empty() && kind != ast::TaskKind::Sporadic) {
    report(diagnostics_, "E-VAL-205", base + ":mit", task.mit.front().span,
           "attribute 'mit' is only valid on sporadic tasks",
           "remove 'mit' or declare the task as 'sporadic'");
  }

  for (const ast::Spanned<ast::TimeValue>& period : task.period) {
    validate_time_value(period.value, base + ":period");
    if (period.value.raw_int <= 0) {
      report(diagnostics_, "E-VAL-206", base + ":period", period.value.span,
             "period must be greater than zero", "set period to a positive time value");
    }
  }
  for (const ast::Spanned<ast::TimeValue>& mit : task.mit) {
    validate_time_value(mit.value, base + ":mit");
    if (mit.value.raw_int <= 0) {
      report(diagnostics_, "E-VAL-207", base + ":mit", mit.value.span,
             "mit must be greater than zero", "set mit to a positive time value");
    }
  }
  for (const ast::Spanned<ast::TimeValue>& deadline : task.deadline) {
    validate_time_value(deadline.value, base + ":deadline");
    if (deadline.value.raw_int <= 0) {
      report(diagnostics_, "E-VAL-208", base + ":deadline", deadline.value.span,
             "deadline must be greater than zero", "set deadline to a positive time value");
    }
  }

  validate_core(task);
  validate_locks(task);

  if (task.time.empty()) {
    report(diagnostics_, "E-VAL-211", base, task.span,
           "task '" + task.id + "' has no 'time' attribute",
           "add a 'time' attribute with intervals and a provenance mode");
  } else {
    for (const ast::TimeDecl& declaration : task.time) {
      validate_time_decl(task, declaration);
    }
  }

  if (kind == ast::TaskKind::Periodic && task.period.empty()) {
    report(diagnostics_, "E-VAL-211", base + ":period", task.span,
           "periodic task '" + task.id + "' is missing its required 'period' attribute",
           "add 'period TIME'");
  }
  if (kind == ast::TaskKind::Sporadic && task.mit.empty()) {
    report(diagnostics_, "E-VAL-211", base + ":mit", task.span,
           "sporadic task '" + task.id + "' is missing its required 'mit' attribute",
           "add 'mit TIME'");
  }

  if (kind == ast::TaskKind::Sporadic) {
    const Span span = task.kind_span.valid() ? task.kind_span : task.span;
    report(diagnostics_, "E-UNS-003", base, span,
           "sporadic task '" + task.id + "' is not supported in v1",
           "sporadic stays rejected; with the user's agreement you may model a periodic "
           "reference scenario with an explicit period such as `period 10ms`. That changes "
           "the model and does not certify the original sporadic system");
  }

  if (kind == ast::TaskKind::Interrupt) {
    const Span span = task.kind_span.valid() ? task.kind_span : task.span;
    report(diagnostics_, "E-UNS-005", base, span,
           "interrupt task '" + task.id +
               "' is lowered as a non-periodic task with the interrupt type marker; v1 does not "
               "yet implement interrupt preemption",
           "no action needed; interrupt preemption semantics are planned", "hint");
  }
}

void Validator::validate_core(const ast::TaskDecl& task) {
  const std::string subject = "task:" + task.id + ":core";
  Span any_span;
  Span set_span;
  std::string set_text;
  std::int64_t set_min = 0;
  for (const ast::CoreSpec& core : task.core) {
    switch (core.kind) {
      case ast::CoreSpec::Kind::Single:
        if (system_.cpus_known && (core.single < 0 || core.single >= system_.cpus)) {
          report(diagnostics_, "E-VAL-209", subject, core.span,
                 "core " + std::to_string(core.single) + " is outside the valid range [0, " +
                     std::to_string(system_.cpus - 1) + "]",
                 "use a core in [0, cpus-1]");
        }
        break;
      case ast::CoreSpec::Kind::Set: {
        if (core.set.empty()) {
          report(diagnostics_, "E-VAL-210", subject, core.span,
                 "the core set must contain at least one core",
                 "list at least one core, or use 'core N' or 'core any'");
        }
        std::set<std::int64_t> seen;
        for (std::size_t index = 0; index < core.set.size(); ++index) {
          const std::int64_t core_id = core.set[index];
          const Span& element_span =
              index < core.element_spans.size() ? core.element_spans[index] : core.span;
          if (!seen.insert(core_id).second) {
            report(diagnostics_, "E-VAL-210", subject, element_span,
                   "the core set contains duplicate core " + std::to_string(core_id),
                   "remove duplicate entries from the core set");
          }
          if (system_.cpus_known && (core_id < 0 || core_id >= system_.cpus)) {
            report(diagnostics_, "E-VAL-209", subject, element_span,
                   "core " + std::to_string(core_id) + " is outside the valid range [0, " +
                       std::to_string(system_.cpus - 1) + "]",
                   "use a core in [0, cpus-1]");
          }
        }
        // A multi-core set is an allocation domain: lowering pins the task to
        // the smallest id and records the set. Silently analysing a set as one
        // core is a trap, so it is a hint in `check` and an error in `rtsl`
        // (like `core any`), while `allocate` explores the set.
        if (seen.size() > 1) {
          set_span = core.span;
          set_min = *seen.begin();
          set_text = "{";
          bool first = true;
          for (std::int64_t core_id : seen) {
            if (!first) {
              set_text += ", ";
            }
            set_text += std::to_string(core_id);
            first = false;
          }
          set_text += "}";
        }
        break;
      }
      case ast::CoreSpec::Kind::Any:
        any_span = core.span;
        break;
    }
  }

  if (any_span.valid()) {
    // `check` only hints, `rtsl` rejects (analysis needs a fixed core), and
    // `allocate` expands the domain before searching.
    if (options_.mode == ValidationOptions::Mode::Rtsl) {
      report(diagnostics_, "E-VAL-218", subject, any_span,
             "task '" + task.id + "' uses 'core any', which requires ptpn allocate",
             "use `ptpn allocate` to expand the core domain before analysis");
    } else if (options_.mode == ValidationOptions::Mode::Check) {
      report(diagnostics_, "E-VAL-218", subject, any_span,
             "task '" + task.id + "' uses 'core any', which requires ptpn allocate",
             "use `ptpn allocate` to expand the core domain before analysis", "hint");
    }
  }

  if (set_span.valid()) {
    // A multi-core set is an allocation domain: lowering pins the task to the
    // smallest id and records the set, and `allocate` explores it. Analysing it
    // as one core is a trap, so `rtsl` warns (the lowered model may be
    // overloaded for reasons the user did not intend) while `check` hints;
    // `allocate` stays silent because it does explore the set.
    const std::string message = "task '" + task.id + "' uses the core set '" + set_text +
                                "'; analysis pins it to core " + std::to_string(set_min) +
                                " instead of exploring the set";
    const std::string fix =
        "use `ptpn allocate` to explore the set, or pin the task to one core";
    if (options_.mode == ValidationOptions::Mode::Rtsl) {
      report(diagnostics_, "E-VAL-218", subject, set_span, message, fix, "warning");
    } else if (options_.mode == ValidationOptions::Mode::Check) {
      report(diagnostics_, "E-VAL-218", subject, set_span, message, fix, "hint");
    }
  }
}

void Validator::validate_locks(const ast::TaskDecl& task) {
  for (const ast::LocksDecl& declaration : task.locks) {
    for (const ast::IdentRef& lock : declaration.locks) {
      source_map_.bind("lock:" + lock.name, lock.span);
      if (lock.name.find("__") != std::string::npos &&
          reported_locks_.insert("reserved:" + lock.name).second) {
        report(diagnostics_, "E-VAL-202", "lock:" + lock.name, lock.span,
               "lock name '" + lock.name + "' contains '__', which is reserved for implicit nodes",
               "rename the lock without '__'");
      }
      if (!has_lock_prefix(lock.name) && reported_locks_.insert("prefix:" + lock.name).second) {
        report(diagnostics_, "E-VAL-014", "lock:" + lock.name, lock.span,
               "Task " + task.id + " uses invalid lock prefix '" + lock.name +
                   "': must start with 'mutex' or 'spin'",
               kLockFix);
      }
    }
  }
}

void Validator::validate_time_decl(const ast::TaskDecl& task, const ast::TimeDecl& declaration) {
  const std::string subject = "task:" + task.id + ":time";

  for (const ast::TimeInterval& interval : declaration.intervals) {
    validate_time_value(interval.min, subject);
    validate_time_value(interval.max, subject);
    if (interval.min.normalized_ms(system_.time_unit) >
        interval.max.normalized_ms(system_.time_unit)) {
      report(diagnostics_, "E-VAL-212", subject, interval.span, "time interval has min > max",
             "ensure each interval is [min, max] with min <= max");
    }
  }

  switch (declaration.provenance.kind) {
    case ast::ProvenanceKind::Estimated:
    case ast::ProvenanceKind::Certified:
      if (declaration.intervals.empty()) {
        report(diagnostics_, "E-VAL-214", subject, declaration.span,
               declaration.provenance.kind == ast::ProvenanceKind::Estimated
                   ? "estimated time is missing its interval(s)"
                   : "certified time is missing its interval(s)",
               "add [min, max] intervals before the provenance keyword");
      }
      break;
    case ast::ProvenanceKind::Measured:
      // check accepts the pre-measurement state; rtsl/allocate reject it.
      if (options_.mode != ValidationOptions::Mode::Check && declaration.intervals.empty()) {
        report(diagnostics_, "E-VAL-213", subject, declaration.span,
               "measured time is missing its interval(s)",
               "fill the time interval in the RTSL source first");
      }
      break;
  }

  if (declaration.provenance.kind == ast::ProvenanceKind::Measured) {
    const ast::MeasureBlock& measure = declaration.provenance.measure;
    report_duplicate_attributes(diagnostics_, measure.runs, subject, "runs");
    report_duplicate_attributes(diagnostics_, measure.timeout, subject, "timeout");
    report_duplicate_attributes(diagnostics_, measure.artifacts, subject, "artifact");
    for (const ast::Spanned<std::int64_t>& runs : measure.runs) {
      if (runs.value <= 0) {
        report(diagnostics_, "E-VAL-220", subject, runs.span,
               "measurement 'runs' must be a positive integer", "set runs to a positive integer");
      }
    }
    for (const ast::Spanned<std::int64_t>& timeout : measure.timeout) {
      if (timeout.value <= 0) {
        report(diagnostics_, "E-VAL-220", subject, timeout.span,
               "measurement 'timeout' must be a positive integer (wall-clock seconds)",
               "set timeout to a positive number of wall-clock seconds");
      }
    }
  }
}

void Validator::validate_schedule_hints() {
  // Per-task and per-core modelling hints that need the whole task list. All of
  // them are non-blocking (hint/warning); they exist so the analysis verdict
  // cannot surprise the author of the model.
  struct TaskInfo {
    std::string id;
    Span span;
    std::int64_t core = 0;
    std::int64_t priority = 100;
    std::int64_t period_ms = 0;
    std::int64_t deadline_ms = 0;
    std::int64_t wcet_ms = 0;
    std::int64_t bcet_ms = 0;
    bool periodic = false;
  };

  std::vector<TaskInfo> infos;
  std::set<std::string> seen;
  for (const ast::TaskDecl& task : program_.tasks) {
    if (!seen.insert(task.id).second) {
      continue;  // duplicate ids are E-VAL-201
    }
    if (task.effective_kind() == ast::TaskKind::Sporadic || task.time.empty() ||
        task.time.front().intervals.empty()) {
      continue;  // no timing to reason about (E-UNS-003 / E-VAL-213)
    }
    TaskInfo info;
    info.id = task.id;
    info.span = task.span;
    if (!task.core.empty() && task.core.front().kind == ast::CoreSpec::Kind::Single) {
      info.core = task.core.front().single;
    } else if (!task.core.empty() && task.core.front().kind == ast::CoreSpec::Kind::Set &&
               !task.core.front().set.empty()) {
      info.core = *std::min_element(task.core.front().set.begin(), task.core.front().set.end());
    }
    if (!task.priority.empty()) {
      info.priority = task.priority.front().value;
    }
    info.periodic = task.effective_kind() == ast::TaskKind::Periodic;
    if (info.periodic && !task.period.empty()) {
      info.period_ms = task.period.front().value.normalized_ms(system_.time_unit);
    }
    if (!task.deadline.empty()) {
      info.deadline_ms = task.deadline.front().value.normalized_ms(system_.time_unit);
    }
    for (const ast::TimeInterval& interval : task.time.front().intervals) {
      info.bcet_ms += interval.min.normalized_ms(system_.time_unit);
      info.wcet_ms += interval.max.normalized_ms(system_.time_unit);
    }

    if (info.periodic && info.deadline_ms > 0 && info.period_ms > 0 &&
        info.deadline_ms > info.period_ms) {
      report(diagnostics_, "E-VAL-222", "task:" + info.id + ":deadline", task.deadline.front().span,
             "the relative deadline (" + std::to_string(info.deadline_ms) +
                 " ms) exceeds the period (" + std::to_string(info.period_ms) +
                 " ms); a job can outlive its next release and the periodic release merges into "
                 "the pending job",
             "make the deadline <= the period or model the queue explicitly", "warning");
    }
    infos.push_back(std::move(info));
  }
  if (infos.empty()) {
    return;
  }

  // Same core, same priority: the tie rule decides who keeps the core.
  std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::string>> by_core_priority;
  for (const TaskInfo& info : infos) {
    by_core_priority[{info.core, info.priority}].push_back(info.id);
  }
  for (const auto& [key, ids] : by_core_priority) {
    if (ids.size() < 2) {
      continue;
    }
    std::string list;
    for (const std::string& id : ids) {
      if (!list.empty()) {
        list += ", ";
      }
      list += id;
    }
    report(diagnostics_, "E-VAL-223", "system:priority", program_.system.span,
           "tasks " + list + " share core " + std::to_string(key.first) + " and priority " +
               std::to_string(key.second) +
               "; equal-priority preemption is decided by the tie rule (default fifo: a running "
               "task keeps the core)",
           "give the tasks distinct priorities, or run with --tie arbitrary to explore every "
           "tie-break",
           "hint");
  }

  // Single-core utilisation above 1 is a static necessary-condition violation.
  std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> core_work;  // core -> (bcet, wcet)
  std::map<std::int64_t, std::int64_t> core_period_sum;                     // for a message hint
  for (const TaskInfo& info : infos) {
    if (!info.periodic || info.period_ms <= 0) {
      continue;
    }
    auto& work = core_work[info.core];
    work.first += info.bcet_ms;
    work.second += info.wcet_ms;
  }
  for (const auto& [core, work] : core_work) {
    double util_max = 0.0;
    double util_min = 0.0;
    for (const TaskInfo& info : infos) {
      if (info.core != core || !info.periodic || info.period_ms <= 0) {
        continue;
      }
      util_max += static_cast<double>(info.wcet_ms) / static_cast<double>(info.period_ms);
      util_min += static_cast<double>(info.bcet_ms) / static_cast<double>(info.period_ms);
    }
    if (util_max <= 1.0) {
      continue;
    }
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%.2f", util_max);
    report(diagnostics_, "E-VAL-224", "system:core" + std::to_string(core), program_.system.span,
           "the periodic tasks on core " + std::to_string(core) +
               " demand more than the core can supply (sum C/T = " + std::string(buffer) +
               " with worst-case execution times" +
               (util_min > 1.0 ? ", and still > 1 with best-case execution times" : "") +
               "); no schedule can meet every deadline",
           "reduce the execution times, lengthen the periods, or move a task to another core",
           "warning");
  }
}

void Validator::validate_edges() {
  std::map<std::string, const ast::EdgeDecl*> first_seen;
  for (const ast::EdgeDecl& edge : program_.edges) {
    const std::vector<std::string> sources = edge.source_names();
    const std::vector<std::string> targets = edge.target_names();
    const std::string subject = ast::canonical_edge_subject(sources, targets);

    if (sources.size() >= 2 && targets.size() >= 2) {
      report(diagnostics_, "E-VAL-216", subject, edge.span,
             "edge connects multiple source tasks to multiple target tasks",
             "split the multi-to-multi edge into explicit intermediate tasks");
    }

    for (const ast::IdentRef& source : edge.sources) {
      if (task_ids_.count(source.name) == 0) {
        report(diagnostics_, "E-VAL-204", subject, source.span,
               "edge references unknown task '" + source.name + "'",
               "add a task named '" + source.name + "' or correct the edge endpoint");
      }
    }
    for (const ast::IdentRef& target : edge.targets) {
      if (task_ids_.count(target.name) == 0) {
        report(diagnostics_, "E-VAL-204", subject, target.span,
               "edge references unknown task '" + target.name + "'",
               "add a task named '" + target.name + "' or correct the edge endpoint");
      }
    }

    const auto [iterator, inserted] = first_seen.emplace(subject, &edge);
    if (!inserted) {
      const Span& first_span = iterator->second->span;
      report(diagnostics_, "E-VAL-203", subject, first_span,
             "duplicate edge declaration (also declared at " + describe_position(edge.span) + ")",
             "remove one of the duplicate edge declarations");
      report(diagnostics_, "E-VAL-203", subject, edge.span,
             "duplicate edge declaration (also declared at " + describe_position(first_span) + ")",
             "remove one of the duplicate edge declarations");
    }
  }

  // Several separate dependency statements with a shared source or target used
  // to lower to the token-consuming bridge semantics by accident: the source's
  // exit token is consumed by whichever bridge fires first (a choice), and a
  // target with several incoming statements is released by whichever source
  // completes first (an OR join). The meaning is easy to miss, so it is now an
  // error: an explicit `a -> {b, c}` / `{a, b} -> c` statement is an AND
  // fork/join, and an explicit `a -> b | c` / `a | b -> c` statement states the
  // token-consuming alternative on purpose.
  std::map<std::string, std::vector<const ast::EdgeDecl*>> outgoing_statements;
  std::map<std::string, std::vector<const ast::EdgeDecl*>> incoming_statements;
  for (const ast::EdgeDecl& edge : program_.edges) {
    const std::vector<std::string> sources = edge.source_names();
    const std::vector<std::string> targets = edge.target_names();
    if (sources.empty() || targets.empty()) {
      continue;
    }
    for (const ast::IdentRef& source : edge.sources) {
      if (task_ids_.count(source.name) != 0) {
        outgoing_statements[source.name].push_back(&edge);
        break;
      }
    }
    for (const ast::IdentRef& target : edge.targets) {
      if (task_ids_.count(target.name) != 0) {
        incoming_statements[target.name].push_back(&edge);
        break;
      }
    }
  }
  // Two statements with the same endpoint sets are a duplicate (E-VAL-203),
  // not an accidental choice: report E-VAL-225 only when the statements
  // actually differ.
  const auto statements_differ = [](const std::vector<const ast::EdgeDecl*>& statements) {
    const auto& first = statements.front();
    for (const ast::EdgeDecl* statement : statements) {
      if (statement->source_names() != first->source_names() ||
          statement->target_names() != first->target_names()) {
        return true;
      }
    }
    return false;
  };
  for (const auto& [source, statements] : outgoing_statements) {
    if (statements.size() < 2 || !statements_differ(statements)) {
      continue;
    }
    report(diagnostics_, "E-VAL-225", "task:" + source, statements.front()->span,
           "task '" + source + "' has " + std::to_string(statements.size()) +
               " separate outgoing edge statements; the exit token would be consumed by whichever "
               "bridge fires first, so the targets are alternatives and only one is released",
           "write one '" + source + " -> {t1, t2}' statement for an AND fork, or '" + source +
               " -> t1 | t2' to state the token-consuming choice on purpose");
  }
  for (const auto& [target, statements] : incoming_statements) {
    if (statements.size() < 2 || !statements_differ(statements)) {
      continue;
    }
    report(diagnostics_, "E-VAL-225", "task:" + target, statements.front()->span,
           "task '" + target + "' has " + std::to_string(statements.size()) +
               " separate incoming edge statements; it would be released by whichever source "
               "completes first and can run once per completion",
           "write one '{s1, s2} -> " + target + "}' statement for an AND join, or 's1 | s2 -> " +
               target + "' to state the token-consuming choice on purpose");
  }
}

void Validator::validate_bindings() {
  for (const ast::BindingDecl& binding : program_.bindings) {
    if (task_ids_.count(binding.task.name) != 0) {
      continue;
    }
    const bool is_start = binding.kind == ast::BindingKind::Start;
    const std::string keyword = is_start ? "start" : "end";
    report(diagnostics_, "E-VAL-204", keyword + ":" + binding.task.name, binding.task.span,
           "'" + keyword + "' binding references unknown task '" + binding.task.name + "'",
           "reference an existing task id");
  }
}

}  // namespace

void validate(const ast::Program& program, SourceMap& source_map, ptpn::Diagnostics& diagnostics,
              const ValidationOptions& options) {
  Validator validator(program, source_map, diagnostics, options);
  validator.run();
}

nlohmann::json collect_pending_measurements(const ast::Program& program,
                                            const SourceMap& source_map) {
  nlohmann::json pending = nlohmann::json::array();
  for (const ast::TaskDecl& task : program.tasks) {
    if (task.time.size() != 1) {
      continue;
    }
    const ast::TimeDecl& declaration = task.time.front();
    if (declaration.provenance.kind != ast::ProvenanceKind::Measured ||
        !declaration.intervals.empty()) {
      continue;
    }
    const std::size_t lock_count = task.lock_names().size();
    const ast::MeasureBlock& measure = declaration.provenance.measure;
    const std::int64_t runs =
        measure.runs.empty() ? ast::kDefaultMeasuredRuns : measure.runs.front().value;
    const std::int64_t timeout =
        measure.timeout.empty() ? ast::kDefaultMeasuredTimeout : measure.timeout.front().value;

    nlohmann::json entry = nlohmann::json::object();
    entry["task"] = task.id;
    entry["expected_segments"] = static_cast<std::int64_t>(2 * lock_count + 1);
    entry["runs"] = runs;
    entry["timeout"] = timeout;

    const std::optional<Span> inject = source_map.span_for("task:" + task.id + ":time");
    if (inject.has_value()) {
      entry["inject"] = nlohmann::json{{"file", inject->file},
                                       {"byte_start", inject->byte_start},
                                       {"byte_end", inject->byte_end},
                                       {"line", inject->line},
                                       {"column", inject->column}};
    }
    pending.push_back(std::move(entry));
  }
  return pending;
}

}  // namespace dsl
