#ifndef DSL_AST_H
#define DSL_AST_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dsl/source_map.h"

namespace dsl {
namespace ast {

// Defaults for `measured by llm` configuration.
inline constexpr std::int64_t kDefaultMeasuredRuns = 20;
inline constexpr std::int64_t kDefaultMeasuredTimeout = 5;

enum class TaskKind { Normal, Periodic, Aperiodic, Sporadic, Interrupt };

const char* task_kind_name(TaskKind kind);

// A time value as written: non-negative integer plus an optional suffix. When
// `unit` is empty the literal is relative to the system `time_unit`. `us` and
// unknown suffixes are kept verbatim so validation can classify them.
struct TimeValue {
  std::int64_t raw_int = 0;
  std::string unit;
  Span span;

  // Converts to the millisecond base. `default_unit` is the system time_unit
  // ("ms" or "s"; an invalid one falls back to "ms"). Returns the raw integer
  // for `us`/unknown suffixes because those are rejected by validation.
  [[nodiscard]] std::int64_t normalized_ms(std::string_view default_unit) const;
};

template <typename T>
struct Spanned {
  T value{};
  Span span;
};

struct IdentRef {
  std::string name;
  Span span;
};

struct CoreSpec {
  enum class Kind { Single, Any, Set };

  Kind kind = Kind::Single;
  std::int64_t single = 0;
  std::vector<std::int64_t> set;
  std::vector<Span> element_spans;
  Span span;          // whole attribute ("core" through the last value token)
  Span keyword_span;  // the "core" keyword
};

struct LocksDecl {
  std::vector<IdentRef> locks;
  Span span;          // whole attribute
  Span keyword_span;  // the "locks" keyword
};

struct TimeInterval {
  TimeValue min;
  TimeValue max;
  Span span;  // the whole "[min, max]" interval
};

enum class ProvenanceKind { Estimated, Certified, Measured };

// Configuration of a `measured by llm { ... }` block. Vectors preserve every
// declaration so duplicate sub-attributes can be reported (E-VAL-219).
struct MeasureBlock {
  std::vector<Spanned<std::int64_t>> runs;
  std::vector<Spanned<std::int64_t>> timeout;
  std::vector<Spanned<std::string>> artifacts;
  Span span;  // the "{ ... }" block
};

struct Provenance {
  ProvenanceKind kind = ProvenanceKind::Estimated;
  Span span;  // keyword through the end of the provenance clause
  std::optional<Spanned<std::string>> by;
  // Only for `certified by STRING { artifact STRING }`.
  std::optional<Spanned<std::string>> artifact;
  MeasureBlock measure;  // only for measured
};

struct TimeDecl {
  bool has_intervals = false;
  std::vector<TimeInterval> intervals;
  Provenance provenance;
  Span span;          // the WHOLE time attribute; this is the injection span
  Span keyword_span;  // the "time" keyword
};

struct TaskDecl {
  std::string id;
  Span id_span;
  std::optional<TaskKind> kind;  // absent means `normal`
  Span kind_span;
  std::vector<Spanned<TimeValue>> period;
  std::vector<Spanned<TimeValue>> deadline;
  std::vector<Spanned<TimeValue>> mit;
  std::vector<Spanned<std::int64_t>> priority;
  std::vector<CoreSpec> core;
  std::vector<LocksDecl> locks;
  std::vector<TimeDecl> time;
  Span span;          // whole declaration ("task" through the closing "}")
  Span keyword_span;  // the "task" keyword

  [[nodiscard]] TaskKind effective_kind() const {
    return kind.value_or(TaskKind::Normal);
  }

  // Distinct lock names declared by this task (implicit registration).
  [[nodiscard]] std::vector<std::string> lock_names() const;
};

struct SystemBlock {
  std::string name;
  Span name_span;
  std::vector<Spanned<std::int64_t>> cpus;
  std::vector<Spanned<std::int64_t>> cores_per_cpu;
  std::vector<Spanned<std::string>> time_unit;
  std::vector<Spanned<std::string>> policy;
  Span span;  // "system" through the closing "}"
};

enum class BindingKind { Start, End };

struct BindingDecl {
  BindingKind kind = BindingKind::Start;
  IdentRef task;
  Span span;
};

struct EdgeDecl {
  std::vector<IdentRef> sources;
  std::vector<IdentRef> targets;
  bool source_braced = false;
  bool target_braced = false;
  // `a -> b | c` / `a | b -> c`: the token-consuming alternative form, written
  // with `|` instead of the braced AND fork/join lists.
  bool alternative = false;
  Span span;        // whole edge declaration
  Span arrow_span;  // the "->" token

  [[nodiscard]] std::vector<std::string> source_names() const;
  [[nodiscard]] std::vector<std::string> target_names() const;
};

struct Program {
  SystemBlock system;
  std::vector<TaskDecl> tasks;
  std::vector<BindingDecl> bindings;
  std::vector<EdgeDecl> edges;
};

// Canonical subject key for an edge: endpoints sorted lexicographically and
// joined with commas, for example "edge:sensor->fusion,log".
std::string canonical_edge_subject(const std::vector<std::string>& sources,
                                   const std::vector<std::string>& targets);

// Canonical implicit-node names.
std::string canonical_fork_name(const std::string& source, const std::vector<std::string>& targets);
std::string canonical_join_name(const std::vector<std::string>& sources, const std::string& target);

inline const char* task_kind_name(TaskKind kind) {
  switch (kind) {
    case TaskKind::Periodic:
      return "periodic";
    case TaskKind::Aperiodic:
      return "aperiodic";
    case TaskKind::Sporadic:
      return "sporadic";
    case TaskKind::Interrupt:
      return "interrupt";
    case TaskKind::Normal:
    default:
      return "normal";
  }
}

inline std::int64_t TimeValue::normalized_ms(std::string_view default_unit) const {
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  std::string_view unit_view = unit;
  if (unit_view.empty()) {
    unit_view = default_unit == "s" ? "s" : "ms";
  }
  if (unit_view == "s") {
    if (raw_int > max / 1000) {
      return max;
    }
    return raw_int * 1000;
  }
  return raw_int;
}

// Largest number of engine ticks (milliseconds) a time value may normalize to.
// The engine stores ticks in a signed 32-bit int and reserves INT_MAX as the
// +infinity sentinel, so finite values must stay strictly below it.
inline constexpr std::int64_t kMaxEngineTicks =
    static_cast<std::int64_t>(std::numeric_limits<int>::max()) - 1;

// True when the value normalizes to a number of engine ticks the signed 32-bit
// engine cannot represent. Validation reports E-VAL-221 and refuses to lower;
// without this check the cast in the lowering stage would silently wrap.
inline bool exceeds_engine_tick_range(const TimeValue& value, std::string_view default_unit) {
  const std::int64_t ticks = value.normalized_ms(default_unit);
  return ticks < 0 || ticks > kMaxEngineTicks;
}

inline std::vector<std::string> TaskDecl::lock_names() const {
  std::vector<std::string> names;
  for (const LocksDecl& declaration : locks) {
    for (const IdentRef& lock : declaration.locks) {
      if (std::find(names.begin(), names.end(), lock.name) == names.end()) {
        names.push_back(lock.name);
      }
    }
  }
  return names;
}

inline std::vector<std::string> EdgeDecl::source_names() const {
  std::vector<std::string> names;
  names.reserve(sources.size());
  for (const IdentRef& source : sources) {
    names.push_back(source.name);
  }
  return names;
}

inline std::vector<std::string> EdgeDecl::target_names() const {
  std::vector<std::string> names;
  names.reserve(targets.size());
  for (const IdentRef& target : targets) {
    names.push_back(target.name);
  }
  return names;
}

inline std::string canonical_edge_subject(const std::vector<std::string>& sources,
                                          const std::vector<std::string>& targets) {
  std::vector<std::string> sorted_sources = sources;
  std::vector<std::string> sorted_targets = targets;
  std::sort(sorted_sources.begin(), sorted_sources.end());
  std::sort(sorted_targets.begin(), sorted_targets.end());
  std::string subject = "edge:";
  for (std::size_t index = 0; index < sorted_sources.size(); ++index) {
    if (index != 0) {
      subject += ",";
    }
    subject += sorted_sources[index];
  }
  subject += "->";
  for (std::size_t index = 0; index < sorted_targets.size(); ++index) {
    if (index != 0) {
      subject += ",";
    }
    subject += sorted_targets[index];
  }
  return subject;
}

inline std::string canonical_fork_name(const std::string& source,
                                       const std::vector<std::string>& targets) {
  std::vector<std::string> sorted_targets = targets;
  std::sort(sorted_targets.begin(), sorted_targets.end());
  std::string name = "__fork_" + source;
  for (const std::string& target : sorted_targets) {
    name += "__" + target;
  }
  return name;
}

inline std::string canonical_join_name(const std::vector<std::string>& sources,
                                       const std::string& target) {
  std::vector<std::string> sorted_sources = sources;
  std::sort(sorted_sources.begin(), sorted_sources.end());
  std::string name = "__join_";
  for (std::size_t index = 0; index < sorted_sources.size(); ++index) {
    if (index != 0) {
      name += "__";
    }
    name += sorted_sources[index];
  }
  name += "__" + target;
  return name;
}

}  // namespace ast
}  // namespace dsl

#endif  // DSL_AST_H
