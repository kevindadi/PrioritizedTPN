#include "app/fill.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/common.h"
#include "app/input.h"
#include "dsl/ast.h"
#include "dsl/validate.h"

namespace app {

namespace {

// The engine's signed 32-bit millisecond range; INT_MAX itself is reserved as
// the +infinity sentinel (docs/rtsl.md), so a usable value is at most INT_MAX-1.
constexpr long long kMaxEngineTick = static_cast<long long>(std::numeric_limits<int>::max()) - 1;

struct Interval {
  long long min = 0;
  long long max = 0;
};

struct FillRequest {
  std::string task;
  std::vector<Interval> intervals;
  int line = 0;
  int column = 0;
};

std::optional<ptpn::SourceLocation> location_from_byte(const std::string& text, std::size_t byte,
                                                       const std::string& file) {
  if (byte > text.size()) {
    byte = text.size();
  }
  ptpn::SourceLocation location;
  location.file = file;
  int line = 1;
  int column = 1;
  for (std::size_t i = 0; i < byte; ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
  location.line = line;
  location.column = column;
  return location;
}

void add_fill_error(ptpn::Diagnostics& diagnostics, const std::string& message,
                    const std::string& subject) {
  diagnostics.add("E-VAL-226", message, subject, std::nullopt,
                  "run 'ptpn check -f model.rtsl' to list the pending tasks and their expected "
                  "segment counts");
}

// Parses "[a,b],[c,d]" or "[ [a,b], [c,d] ]" into intervals. Returns false and
// fills `error` on the first malformed value.
bool parse_intervals_text(const std::string& text, std::vector<Interval>& out, std::string& error) {
  std::size_t i = 0;
  const auto skip_ws_at = [&](std::size_t& position) {
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])) != 0) {
      ++position;
    }
  };
  const auto skip_ws = [&]() { skip_ws_at(i); };
  const auto expect = [&](char c) {
    skip_ws();
    if (i >= text.size() || text[i] != c) {
      return false;
    }
    ++i;
    return true;
  };
  const auto parse_number = [&](long long& value) {
    skip_ws();
    const std::size_t start = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
      ++i;
    }
    if (i == start) {
      return false;
    }
    try {
      value = std::stoll(text.substr(start, i - start));
    } catch (const std::exception&) {
      return false;
    }
    return true;
  };

  skip_ws();
  if (i >= text.size() || text[i] != '[') {
    error = "expected '[' to start an interval";
    return false;
  }
  std::size_t lookahead = i + 1;
  skip_ws_at(lookahead);
  const bool outer = lookahead < text.size() && text[lookahead] == '[';
  if (outer) {
    i = lookahead;
  }

  while (true) {
    Interval interval;
    if (!expect('[')) {
      error = "expected '[' to start an interval";
      return false;
    }
    if (!parse_number(interval.min)) {
      error = "expected the interval's lower bound";
      return false;
    }
    if (!expect(',')) {
      error = "expected ',' between the interval bounds";
      return false;
    }
    if (!parse_number(interval.max)) {
      error = "expected the interval's upper bound";
      return false;
    }
    if (!expect(']')) {
      error = "expected ']' after the interval bounds";
      return false;
    }
    out.push_back(interval);
    skip_ws();
    if (i < text.size() && text[i] == ',') {
      ++i;
      continue;
    }
    break;
  }
  if (outer && !expect(']')) {
    error = "expected ']' to close the interval list";
    return false;
  }
  skip_ws();
  if (i != text.size()) {
    error = "unexpected trailing content after the interval list";
    return false;
  }
  return !out.empty();
}

// Reads and structurally checks the values JSON. Task-level problems (unknown
// task, wrong count, bad bounds) are reported by the caller, which has the
// model.
std::optional<std::vector<FillRequest>> read_values_json(const std::string& path,
                                                         ptpn::Diagnostics& diagnostics) {
  std::ifstream stream(path);
  if (!stream) {
    ptpn::SourceLocation location;
    location.file = path;
    diagnostics.add("E-SYN-002", "failed to open values file: " + path, std::nullopt,
                    std::move(location), "check that the values path exists and is readable");
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = buffer.str();

  nlohmann::json values;
  try {
    values = nlohmann::json::parse(text);
  } catch (const nlohmann::json::parse_error& error) {
    diagnostics.add("E-SYN-001", error.what(), std::nullopt,
                    location_from_byte(text, error.byte, path),
                    "fix the JSON syntax near the reported line and column");
    return std::nullopt;
  } catch (const std::exception& error) {
    ptpn::SourceLocation location;
    location.file = path;
    diagnostics.add("E-VAL-033", error.what(), std::nullopt, std::move(location),
                    "check that every JSON field has the expected type and shape");
    return std::nullopt;
  }

  if (!values.is_object()) {
    add_fill_error(diagnostics,
                   "the values file must be a JSON object mapping task names to interval "
                   "lists: {\"task\": [[min, max], ...]}",
                   "values:" + path);
    return std::nullopt;
  }

  std::vector<FillRequest> requests;
  for (auto it = values.begin(); it != values.end(); ++it) {
    FillRequest request;
    request.task = it.key();
    const nlohmann::json& list = it.value();
    if (!list.is_array() || list.empty()) {
      add_fill_error(diagnostics,
                     "task '" + request.task +
                         "' must map to a non-empty array of [min, max] intervals",
                     "task:" + request.task);
      return std::nullopt;
    }
    for (const nlohmann::json& entry : list) {
      if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_integer() ||
          !entry[1].is_number_integer()) {
        add_fill_error(diagnostics,
                       "task '" + request.task +
                           "' has an interval that is not a [min, max] integer pair",
                       "task:" + request.task);
        return std::nullopt;
      }
      request.intervals.push_back(
          Interval{entry[0].get<long long>(), entry[1].get<long long>()});
    }
    requests.push_back(std::move(request));
  }
  if (requests.empty()) {
    add_fill_error(diagnostics, "the values file contains no tasks", "values:" + path);
    return std::nullopt;
  }
  return requests;
}

// Validates the requested intervals against the task's declared structure.
bool check_request(const FillRequest& request, const dsl::ast::TaskDecl& task,
                   std::size_t expected_segments, ptpn::Diagnostics& diagnostics) {
  if (request.intervals.size() != expected_segments) {
    add_fill_error(diagnostics,
                   "task '" + request.task + "' needs " + std::to_string(expected_segments) +
                       " interval(s) (" + std::to_string(task.lock_names().size()) +
                       " lock(s): pre-critical, one critical section per lock, post-critical), got " +
                       std::to_string(request.intervals.size()),
                   "task:" + request.task);
    return false;
  }
  for (const Interval& interval : request.intervals) {
    if (interval.min < 0 || interval.max < 0 || interval.min > interval.max) {
      add_fill_error(diagnostics,
                     "task '" + request.task + "' has an interval with min > max or a negative "
                     "bound: [" + std::to_string(interval.min) + ", " +
                         std::to_string(interval.max) + "]",
                     "task:" + request.task);
      return false;
    }
    if (interval.max > kMaxEngineTick) {
      add_fill_error(diagnostics,
                     "task '" + request.task + "' has a value above the engine's signed 32-bit "
                     "millisecond range (at most " + std::to_string(kMaxEngineTick) + ")",
                     "task:" + request.task);
      return false;
    }
  }
  return true;
}

std::string format_intervals(const std::vector<Interval>& intervals, bool milliseconds_suffix) {
  const auto format_value = [&](long long value) {
    std::string text = std::to_string(value);
    if (milliseconds_suffix) {
      text += "ms";
    }
    return text;
  };
  std::ostringstream out;
  if (intervals.size() == 1) {
    out << "[" << format_value(intervals.front().min) << ", "
        << format_value(intervals.front().max) << "]";
    return out.str();
  }
  out << "[ ";
  for (std::size_t i = 0; i < intervals.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << "[" << format_value(intervals[i].min) << ", " << format_value(intervals[i].max) << "]";
  }
  out << " ]";
  return out.str();
}

struct Replacement {
  std::size_t start = 0;
  std::size_t end = 0;
  std::string text;
};

// Applies replacements from the end of the file backwards so earlier byte
// offsets stay valid.
std::string apply_replacements(const std::string& source, std::vector<Replacement> replacements) {
  std::sort(replacements.begin(), replacements.end(),
            [](const Replacement& left, const Replacement& right) {
              return left.start > right.start;
            });
  std::string patched = source;
  for (const Replacement& replacement : replacements) {
    if (replacement.start > patched.size() || replacement.end > patched.size() ||
        replacement.end < replacement.start) {
      continue;
    }
    patched.replace(replacement.start, replacement.end - replacement.start, replacement.text);
  }
  return patched;
}

// Without the diagnostics transport every failure is rendered as plain text on
// stderr, matching `ptpn check`.
int report_failure(ptpn::Diagnostics* diagnostics, ptpn::Diagnostics& collector) {
  if (diagnostics != nullptr) {
    return collector.exit_code();
  }
  print_legacy_diagnostics(collector);
  return 1;
}

}  // namespace

int run_rtsl_fill(const FillOptions& options, ptpn::Diagnostics* diagnostics) {
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
  const std::string original = source.value();

  RtslDocument document = parse_and_validate_rtsl(original, options.input_file,
                                                  dsl::ValidationOptions::Mode::Check, collector);
  if (!document.valid) {
    return report_failure(diagnostics, collector);
  }

  // Collect the requested fills from --values or --task/--intervals.
  std::vector<FillRequest> requests;
  if (!options.values_file.empty()) {
    std::optional<std::vector<FillRequest>> parsed = read_values_json(options.values_file, collector);
    if (!parsed.has_value()) {
      return report_failure(diagnostics, collector);
    }
    requests = std::move(parsed.value());
  } else if (!options.task.empty()) {
    FillRequest request;
    request.task = options.task;
    std::string error;
    if (!parse_intervals_text(options.intervals, request.intervals, error)) {
      add_fill_error(collector,
                     "cannot parse the --intervals value '" + options.intervals + "': " + error,
                     "task:" + request.task);
      return report_failure(diagnostics, collector);
    }
    requests.push_back(std::move(request));
  } else {
    add_fill_error(collector,
                   "no timing supplied: pass --values FILE, or --task NAME with --intervals "
                   "\"[min,max],[min,max]\"",
                   "values");
    return report_failure(diagnostics, collector);
  }

  const bool milliseconds_suffix =
      !document.program.system.time_unit.empty() &&
      document.program.system.time_unit.front().value == "s";

  std::vector<Replacement> replacements;
  nlohmann::json filled = nlohmann::json::array();
  for (const FillRequest& request : requests) {
    const auto task_it = std::find_if(document.program.tasks.begin(), document.program.tasks.end(),
                                      [&](const dsl::ast::TaskDecl& task) {
                                        return task.id == request.task;
                                      });
    if (task_it == document.program.tasks.end()) {
      add_fill_error(collector, "task '" + request.task + "' is not declared in the model",
                     "task:" + request.task);
      return report_failure(diagnostics, collector);
    }
    const dsl::ast::TaskDecl& task = *task_it;
    if (task.time.size() != 1) {
      add_fill_error(collector,
                     "task '" + request.task + "' does not declare exactly one time attribute",
                     "task:" + request.task);
      return report_failure(diagnostics, collector);
    }
    const dsl::ast::TimeDecl& declaration = task.time.front();
    const bool pending = declaration.provenance.kind == dsl::ast::ProvenanceKind::Measured &&
                         declaration.intervals.empty();
    if (!pending && !options.overwrite) {
      add_fill_error(collector,
                     "task '" + request.task +
                         "' already has intervals; pass --overwrite to replace them",
                     "task:" + request.task);
      return report_failure(diagnostics, collector);
    }

    const std::size_t expected_segments = 2 * task.lock_names().size() + 1;
    if (!check_request(request, task, expected_segments, collector)) {
      return report_failure(diagnostics, collector);
    }

    // Keep the provenance clause verbatim so `runs`, `timeout` and `artifact`
    // survive the rewrite.
    const dsl::Span& provenance_span = declaration.provenance.span;
    std::string provenance;
    if (provenance_span.valid() && provenance_span.byte_end <= original.size()) {
      provenance = original.substr(provenance_span.byte_start, provenance_span.length());
    }

    std::string text = "time " + format_intervals(request.intervals, milliseconds_suffix);
    if (!provenance.empty()) {
      text += " " + provenance;
    }

    if (!declaration.span.valid() || declaration.span.byte_end > original.size()) {
      collector.add("E-INT-001",
                    "task '" + request.task + "' has no usable source span for its time attribute",
                    "system:internal", std::nullopt, "report this as an internal error");
      return report_failure(diagnostics, collector);
    }
    replacements.push_back(
        Replacement{declaration.span.byte_start, declaration.span.byte_end, std::move(text)});

    nlohmann::json entry = nlohmann::json::object();
    entry["task"] = request.task;
    entry["intervals"] = nlohmann::json::array();
    for (const Interval& interval : request.intervals) {
      entry["intervals"].push_back(nlohmann::json::array({interval.min, interval.max}));
    }
    entry["line"] = declaration.span.line;
    entry["column"] = declaration.span.column;
    filled.push_back(std::move(entry));
  }

  const std::string patched = apply_replacements(original, std::move(replacements));

  // The patched text must parse and validate: fill never writes an invalid
  // model. The re-parse also yields the remaining pending list.
  ptpn::Diagnostics check;
  RtslDocument patched_document = parse_and_validate_rtsl(
      patched, options.input_file, dsl::ValidationOptions::Mode::Check, check);
  if (!patched_document.valid) {
    collector.add("E-INT-001",
                  "the filled source does not validate; no file was written",
                  "system:internal", std::nullopt,
                  "report this as an internal error (fill produced an invalid model)");
    return report_failure(diagnostics, collector);
  }

  nlohmann::json remaining = nlohmann::json::array();
  for (const nlohmann::json& entry :
       dsl::collect_pending_measurements(patched_document.program, patched_document.source_map)) {
    remaining.push_back(entry.at("task"));
  }

  // Destination: --in-place > --output > stdout (or the envelope when the
  // diagnostics transport owns stdout).
  std::string destination = "stdout";
  if (options.in_place) {
    destination = options.input_file;
  } else if (!options.output_file.empty()) {
    destination = options.output_file;
  }

  bool wrote_file = false;
  if (destination != "stdout") {
    const std::string temporary = destination + ".ptpn-fill.tmp";
    {
      std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      if (!stream || !(stream << patched) || !stream.flush()) {
        collector.add("E-INT-002", "failed to write the filled source to: " + destination,
                      "output:" + destination, std::nullopt,
                      "check that the output path is writable");
        std::remove(temporary.c_str());
        return report_failure(diagnostics, collector);
      }
    }
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
      collector.add("E-INT-002", "failed to replace the output file: " + destination,
                    "output:" + destination, std::nullopt,
                    "check that the output path is writable");
      std::remove(temporary.c_str());
      return report_failure(diagnostics, collector);
    }
    wrote_file = true;
  }

  nlohmann::json result = nlohmann::json::object();
  result["filled"] = std::move(filled);
  result["remaining_pending"] = std::move(remaining);
  result["output"] = destination;
  if (diagnostics != nullptr) {
    if (!wrote_file) {
      // The envelope owns stdout, so hand the patched source back through it.
      result["source"] = patched;
    }
    collector.set_result(std::move(result));
    if (collector.exit_code() != 0) {
      return collector.exit_code();
    }
    return 0;
  }

  if (wrote_file) {
    std::cout << "filled " << requests.size() << " task(s) into " << destination << '\n';
  } else {
    std::cout << patched;
  }
  return 0;
}

}  // namespace app
