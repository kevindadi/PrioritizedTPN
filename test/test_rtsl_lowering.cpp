#include <algorithm>
#include <cstddef>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "dsl/ast.h"
#include "dsl/lower.h"
#include "dsl/source_map.h"
#include "dsl/validate.h"
#include "model/tdg.h"
#include "export/tdg_dot.h"
#include "export/tdg_json_writer.h"
#include "test_rtsl_helpers.h"

namespace {

using dsl::ValidationOptions;
using rtsl_test::count_code;
using rtsl_test::find_diagnostic;

const TaskNode* task_node(const tdg::TDG& tdg, const std::string& name) {
  const auto it = tdg.nodes_type.find(name);
  if (it == tdg.nodes_type.end()) {
    return nullptr;
  }
  return std::get_if<TaskNode>(&it->second);
}

const ForkTask* fork_node(const tdg::TDG& tdg, const std::string& name) {
  const auto it = tdg.nodes_type.find(name);
  if (it == tdg.nodes_type.end()) {
    return nullptr;
  }
  return std::get_if<ForkTask>(&it->second);
}

const JoinTask* join_node(const tdg::TDG& tdg, const std::string& name) {
  const auto it = tdg.nodes_type.find(name);
  if (it == tdg.nodes_type.end()) {
    return nullptr;
  }
  return std::get_if<JoinTask>(&it->second);
}

std::set<std::pair<std::string, std::string>> edge_set(const tdg::TDG& tdg) {
  std::set<std::pair<std::string, std::string>> edges;
  for (const auto& edge : tdg.tdg_edges) {
    edges.emplace(edge.source, edge.target);
  }
  return edges;
}

std::optional<tdg::TDG> lower_with_mode(const std::string& source, ptpn::Diagnostics& diagnostics,
                                        dsl::SourceMap& source_map, ValidationOptions::Mode mode) {
  dsl::ast::Program program = rtsl_test::parse_source(source, diagnostics, source_map);
  ValidationOptions options;
  options.mode = mode;
  dsl::validate(program, source_map, diagnostics, options);
  if (!diagnostics.ok()) {
    return std::nullopt;
  }
  return dsl::lower(program, source_map, diagnostics, mode);
}

std::string read_text(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

const char* const kAppendix = R"(system demo {
  cpus 4
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task sensor: periodic {
    period 100ms
    deadline 100ms
    core 0
    time [2, 3] estimated by "user spec sheet"
  }

  task fusion: aperiodic {
    core {1, 2, 3}
    locks mutex1
    time [ [1, 2], [3, 4], [1, 2] ] measured by llm { runs 20, timeout 5 }
  }

  task control: interrupt {
    core 0
    time [1, 1] estimated
  }

  task display: aperiodic {
    core 2
    time [1, 1] estimated
  }

  task log: normal {
    core 2
    time [1, 2] estimated
  }

  start sensor
  start control
  end display

  sensor -> {fusion, log}
  {fusion, log} -> display
}
)";

}  // namespace

TEST(RtslLowering, AppendixShapeMapsToTdg) {
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const std::optional<tdg::TDG> lowered =
      lower_with_mode(kAppendix, diagnostics, source_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(lowered.has_value())
      << (diagnostics.empty() ? "" : diagnostics.entries().front().message);
  const tdg::TDG& tdg = *lowered;

  EXPECT_EQ(tdg.num_cpus, 4);
  EXPECT_EQ(tdg.cores_per_cpu, 1);
  EXPECT_EQ(tdg.policy, SchedulePolicy::FIXED);
  ASSERT_EQ(tdg.all_task.size(), 5U);
  ASSERT_EQ(tdg.start_tasks.size(), 2U);
  EXPECT_EQ(tdg.start_tasks[0].task, "control");
  EXPECT_EQ(tdg.start_tasks[0].tokens, 1);
  EXPECT_EQ(tdg.start_tasks[1].task, "sensor");
  ASSERT_EQ(tdg.end_tasks.size(), 1U);
  EXPECT_EQ(tdg.end_tasks[0], "display");
  ASSERT_EQ(tdg.periodic_tasks.size(), 1U);
  EXPECT_EQ(tdg.periodic_tasks[0].task, "sensor");
  EXPECT_EQ(tdg.periodic_tasks[0].period, 100);

  const TaskNode* sensor = task_node(tdg, "sensor");
  ASSERT_NE(sensor, nullptr);
  EXPECT_EQ(sensor->core, 0);
  EXPECT_EQ(sensor->priority, 100);
  EXPECT_EQ(sensor->deadline, 100);
  EXPECT_EQ(sensor->time, (std::vector<std::pair<int, int>>{{2, 3}}));
  EXPECT_EQ(sensor->task_type, TaskType::NORMAL);
  EXPECT_EQ(tdg.task_allowed_cores.at("sensor"), (std::vector<int>{0}));

  const TaskNode* fusion = task_node(tdg, "fusion");
  ASSERT_NE(fusion, nullptr);
  EXPECT_EQ(fusion->core, 1);
  EXPECT_EQ(fusion->time, (std::vector<std::pair<int, int>>{{1, 2}, {3, 4}, {1, 2}}));
  EXPECT_EQ(fusion->lock, (std::vector<std::string>{"mutex1"}));
  EXPECT_EQ(tdg.task_allowed_cores.at("fusion"), (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(tdg.task_locks_map.at("fusion"), (std::vector<std::string>{"mutex1"}));
  EXPECT_EQ(tdg.lock_set.count("mutex1"), 1U);

  const TaskNode* control = task_node(tdg, "control");
  ASSERT_NE(control, nullptr);
  EXPECT_EQ(control->task_type, TaskType::INTERRUPT);

  const TaskNode* log = task_node(tdg, "log");
  ASSERT_NE(log, nullptr);
  EXPECT_EQ(log->core, 2);

  ASSERT_NE(fork_node(tdg, "__fork_sensor__fusion__log"), nullptr);
  ASSERT_NE(join_node(tdg, "__join_fusion__log__display"), nullptr);
  EXPECT_EQ(tdg.nodes_type.size(), 7U);

  const std::set<std::pair<std::string, std::string>> expected_edges = {
      {"sensor", "__fork_sensor__fusion__log"}, {"__fork_sensor__fusion__log", "fusion"},
      {"__fork_sensor__fusion__log", "log"},    {"fusion", "__join_fusion__log__display"},
      {"log", "__join_fusion__log__display"},   {"__join_fusion__log__display", "display"},
  };
  EXPECT_EQ(edge_set(tdg), expected_edges);
}

TEST(RtslLowering, ImplicitNamesDependOnlyOnEndpoints) {
  const std::string first = rtsl_test::system_block(R"(
  task a: normal { time [1, 2] estimated }
  task b: normal { time [1, 2] estimated }
  task c: normal { time [1, 2] estimated }
  a -> {c, b}
)");
  const std::string second = rtsl_test::system_block(R"(
  task a: normal { time [1, 2] estimated }
  task b: normal { time [1, 2] estimated }
  task c: normal { time [1, 2] estimated }
  task unrelated: normal { time [1, 2] estimated }
  a -> {b, c}
  unrelated -> a
)");

  ptpn::Diagnostics first_diagnostics;
  dsl::SourceMap first_map;
  const auto first_tdg =
      lower_with_mode(first, first_diagnostics, first_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(first_tdg.has_value());

  ptpn::Diagnostics second_diagnostics;
  dsl::SourceMap second_map;
  const auto second_tdg =
      lower_with_mode(second, second_diagnostics, second_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(second_tdg.has_value());

  ASSERT_NE(fork_node(*first_tdg, "__fork_a__b__c"), nullptr);
  ASSERT_NE(fork_node(*second_tdg, "__fork_a__b__c"), nullptr);
  EXPECT_EQ(first_tdg->nodes_type.size(), 4U);
  EXPECT_EQ(second_tdg->nodes_type.size(), 5U);
}

TEST(RtslLowering, UnitsAreNormalizedToMilliseconds) {
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const std::string source = R"(system s {
  cpus 1
  time_unit s
  task bare: periodic { period 2 core 0 time [1, 2] estimated }
  task suffixed: normal { core 0 time [1s, 2000ms] estimated }
  start bare
  start suffixed
}
)";
  const auto lowered =
      lower_with_mode(source, diagnostics, source_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(lowered.has_value());
  EXPECT_EQ(task_node(*lowered, "bare")->time, (std::vector<std::pair<int, int>>{{1000, 2000}}));
  EXPECT_EQ(task_node(*lowered, "suffixed")->time,
            (std::vector<std::pair<int, int>>{{1000, 2000}}));
  ASSERT_EQ(lowered->periodic_tasks.size(), 1U);
  EXPECT_EQ(lowered->periodic_tasks[0].period, 2000);
}

TEST(RtslLowering, SingleElementSetsFoldToPlainEdges) {
  const std::string source = rtsl_test::system_block(R"(
  task a: normal { time [1, 2] estimated }
  task b: normal { time [1, 2] estimated }
  task c: normal { time [1, 2] estimated }
  task d: normal { time [1, 2] estimated }
  a -> {b}
  {b} -> c
  {c} -> {d}
)");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const auto lowered =
      lower_with_mode(source, diagnostics, source_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(lowered.has_value());
  EXPECT_EQ(lowered->nodes_type.size(), 4U);
  EXPECT_EQ(edge_set(*lowered),
            (std::set<std::pair<std::string, std::string>>{{"a", "b"}, {"b", "c"}, {"c", "d"}}));
}

TEST(RtslLowering, ProvenanceMapsToWcetFields) {
  const std::string source = rtsl_test::system_block(R"(
  task estimated_note: normal { time [1, 2] estimated by "spec sheet" }
  task estimated_plain: normal { time [1, 2] estimated }
  task cert_task: normal { time [1, 2] certified by "OTAWA" { artifact "sha256:abc" } }
  task meas_task: normal {
    time [1, 2] measured by llm { runs 3, timeout 4, artifact "trace.bin" }
  }
)");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const auto lowered =
      lower_with_mode(source, diagnostics, source_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(lowered.has_value());

  const auto& estimated_note = lowered->task_wcet_provenance.at("estimated_note");
  EXPECT_EQ(estimated_note.kind, "estimated");
  EXPECT_EQ(estimated_note.method, "spec sheet");
  EXPECT_TRUE(estimated_note.artifact.empty());

  const auto& estimated_plain = lowered->task_wcet_provenance.at("estimated_plain");
  EXPECT_EQ(estimated_plain.kind, "estimated");
  EXPECT_EQ(estimated_plain.method, "user estimate");

  const auto& certified = lowered->task_wcet_provenance.at("cert_task");
  EXPECT_EQ(certified.kind, "certified");
  EXPECT_EQ(certified.method, "OTAWA");
  EXPECT_EQ(certified.artifact, "sha256:abc");

  const auto& measured = lowered->task_wcet_provenance.at("meas_task");
  EXPECT_EQ(measured.kind, "measured");
  EXPECT_EQ(measured.method, "wall-clock repeated runs (prototype; not certified)");
  EXPECT_EQ(measured.artifact, "trace.bin");
}

TEST(RtslLowering, MeasuredWithoutIntervalsIsRejectedOutsideCheckMode) {
  const std::string source = rtsl_test::system_block(R"(
  task pending: normal { time measured by llm { runs 3, timeout 2 } }
)");

  ptpn::Diagnostics check_diagnostics;
  dsl::SourceMap check_map;
  rtsl_test::check_source(source, check_diagnostics, check_map, ValidationOptions{});
  EXPECT_TRUE(check_diagnostics.ok());
  EXPECT_EQ(count_code(check_diagnostics, "E-VAL-213"), 0U);

  for (const ValidationOptions::Mode mode :
       {ValidationOptions::Mode::Rtsl, ValidationOptions::Mode::Allocate}) {
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    ValidationOptions options;
    options.mode = mode;
    const dsl::ast::Program program =
        rtsl_test::check_source(source, diagnostics, source_map, options);
    const ptpn::Diagnostic* pending = find_diagnostic(diagnostics, "E-VAL-213");
    ASSERT_NE(pending, nullptr);
    EXPECT_EQ(pending->severity, "error");
    EXPECT_EQ(pending->fix.value_or(""), "fill the time interval in the RTSL source first");
    EXPECT_FALSE(diagnostics.ok());
    EXPECT_FALSE(dsl::lower(program, source_map, diagnostics, mode).has_value());
  }
}

TEST(RtslLowering, CoreAnySeverityDependsOnMode) {
  const std::string source = rtsl_test::system_block(R"(
  task a: normal { core any time [1, 2] estimated }
)",
                                                     "  cpus 3\n");

  ptpn::Diagnostics check_diagnostics;
  dsl::SourceMap check_map;
  rtsl_test::check_source(source, check_diagnostics, check_map, ValidationOptions{});
  const ptpn::Diagnostic* hint = find_diagnostic(check_diagnostics, "E-VAL-218");
  ASSERT_NE(hint, nullptr);
  EXPECT_EQ(hint->severity, "hint");
  EXPECT_TRUE(check_diagnostics.ok());

  ptpn::Diagnostics rtsl_diagnostics;
  dsl::SourceMap rtsl_map;
  ValidationOptions rtsl_options;
  rtsl_options.mode = ValidationOptions::Mode::Rtsl;
  rtsl_test::check_source(source, rtsl_diagnostics, rtsl_map, rtsl_options);
  const ptpn::Diagnostic* error = find_diagnostic(rtsl_diagnostics, "E-VAL-218");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->severity, "error");
  EXPECT_EQ(error->fix.value_or(""),
            "use `ptpn allocate` to expand the core domain before analysis");
  EXPECT_FALSE(rtsl_diagnostics.ok());

  ptpn::Diagnostics allocate_diagnostics;
  dsl::SourceMap allocate_map;
  const auto lowered = lower_with_mode(source, allocate_diagnostics, allocate_map,
                                       ValidationOptions::Mode::Allocate);
  ASSERT_TRUE(lowered.has_value());
  EXPECT_EQ(count_code(allocate_diagnostics, "E-VAL-218"), 0U);
  EXPECT_EQ(task_node(*lowered, "a")->core, 0);
  EXPECT_EQ(lowered->task_allowed_cores.at("a"), (std::vector<int>{0, 1, 2}));
}

TEST(RtslLowering, CoreSetLowersToMinimumWithSortedDomain) {
  const std::string source = rtsl_test::system_block(R"(
  task a: normal { core {3, 1, 2} time [1, 2] estimated }
)");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const auto lowered =
      lower_with_mode(source, diagnostics, source_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(lowered.has_value());
  EXPECT_EQ(task_node(*lowered, "a")->core, 1);
  EXPECT_EQ(lowered->task_allowed_cores.at("a"), (std::vector<int>{1, 2, 3}));
}

TEST(RtslLowering, SegmentCountMismatchReportsEval015WithLocation) {
  const std::string source = rtsl_test::system_block(R"(
  task a: normal {
    core 0
    locks mutex1
    time [1, 2] estimated
  }
  start a
)");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const dsl::ast::Program program =
      rtsl_test::check_source(source, diagnostics, source_map, ValidationOptions{});

  EXPECT_FALSE(
      dsl::lower(program, source_map, diagnostics, ValidationOptions::Mode::Rtsl).has_value());
  const ptpn::Diagnostic* mismatch = find_diagnostic(diagnostics, "E-VAL-015");
  ASSERT_NE(mismatch, nullptr);
  EXPECT_EQ(mismatch->subject.value_or(""), "task:a");
  ASSERT_TRUE(mismatch->location.has_value());
  EXPECT_EQ(mismatch->location->file, "test.rtsl");
  EXPECT_EQ(mismatch->location->line, 4);
  EXPECT_NE(mismatch->message.find("1 lock(s) but 1 time interval(s) (expected 3)"),
            std::string::npos);
}

TEST(RtslLowering, LocksAreDeduplicatedAndImplicitlyRegistered) {
  const std::string source = rtsl_test::system_block(R"(
  task a: normal {
    locks mutex2, mutex1, mutex2
    time [ [1, 1], [2, 2], [1, 1], [2, 2], [1, 1] ] estimated
  }
)");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const auto lowered =
      lower_with_mode(source, diagnostics, source_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(lowered.has_value());
  EXPECT_EQ(task_node(*lowered, "a")->lock, (std::vector<std::string>{"mutex2", "mutex1"}));
  EXPECT_EQ(lowered->task_locks_map.at("a"), (std::vector<std::string>{"mutex2", "mutex1"}));
  EXPECT_EQ(lowered->lock_set, (std::set<std::string>{"mutex1", "mutex2"}));
  EXPECT_EQ(task_node(*lowered, "a")->time.size(), 5U);
}

TEST(RtslLowering, JsonExportIsDeterministicAndRoundTrips) {
  ptpn::Diagnostics first_diagnostics;
  dsl::SourceMap first_map;
  const auto first =
      lower_with_mode(kAppendix, first_diagnostics, first_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(first.has_value());

  ptpn::Diagnostics second_diagnostics;
  dsl::SourceMap second_map;
  const auto second =
      lower_with_mode(kAppendix, second_diagnostics, second_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(second.has_value());

  const std::string first_json = exporting::tdg_to_json(*first).dump(2) + "\n";
  const std::string second_json = exporting::tdg_to_json(*second).dump(2) + "\n";
  EXPECT_EQ(first_json, second_json);

  tdg::TDG round_tripped;
  round_tripped.parse_json_string(first_json);
  EXPECT_EQ(exporting::tdg_to_json(round_tripped).dump(2) + "\n", first_json);
}

TEST(RtslLowering, GoldenJsonAndDotAreByteStable) {
#ifndef __GLIBCXX__
  GTEST_SKIP() << "golden files are generated with libstdc++";
#endif
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const std::string source = read_text("test/golden/rtsl/appendix.rtsl");
  ASSERT_FALSE(source.empty());
  const auto lowered =
      lower_with_mode(source, diagnostics, source_map, ValidationOptions::Mode::Rtsl);
  ASSERT_TRUE(lowered.has_value());

  const std::string json = exporting::tdg_to_json(*lowered).dump(2) + "\n";
  EXPECT_EQ(json, read_text("test/golden/rtsl/appendix.tdg.json"))
      << "regenerate test/golden/rtsl/appendix.tdg.json if the change is intentional";
  EXPECT_EQ(tdg::to_dot_string(*lowered), read_text("test/golden/rtsl/appendix.tdg.dot"))
      << "regenerate test/golden/rtsl/appendix.tdg.dot if the change is intentional";
}
