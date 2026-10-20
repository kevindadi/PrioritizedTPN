#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "common/diagnostics.h"
#include "dsl/ast.h"
#include "dsl/parser.h"
#include "dsl/source_map.h"
#include "test_rtsl_helpers.h"

namespace {

const char* const kAppendixA = R"(system demo {
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
    core any
    time [1, 2] estimated
  }

  start sensor
  start control
  end display

  sensor -> {fusion, log}
  {fusion, log} -> display
}
)";

TEST(RtslParser, ParsesTheFullAppendixAModel) {
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const dsl::ast::Program program = rtsl_test::parse_source(kAppendixA, diagnostics, source_map);
  ASSERT_TRUE(diagnostics.empty()) << diagnostics.entries().front().message;

  ASSERT_EQ(program.system.name, "demo");
  ASSERT_EQ(program.system.cpus.size(), 1U);
  EXPECT_EQ(program.system.cpus[0].value, 4);
  ASSERT_EQ(program.system.cores_per_cpu.size(), 1U);
  EXPECT_EQ(program.system.cores_per_cpu[0].value, 1);
  ASSERT_EQ(program.system.time_unit.size(), 1U);
  EXPECT_EQ(program.system.time_unit[0].value, "ms");
  ASSERT_EQ(program.system.policy.size(), 1U);
  EXPECT_EQ(program.system.policy[0].value, "fixed");

  ASSERT_EQ(program.tasks.size(), 5U);
  EXPECT_EQ(program.tasks[0].id, "sensor");
  EXPECT_EQ(program.tasks[0].effective_kind(), dsl::ast::TaskKind::Periodic);
  ASSERT_EQ(program.tasks[0].period.size(), 1U);
  EXPECT_EQ(program.tasks[0].period[0].value.raw_int, 100);
  EXPECT_EQ(program.tasks[0].period[0].value.unit, "ms");
  ASSERT_EQ(program.tasks[0].time.size(), 1U);
  ASSERT_EQ(program.tasks[0].time[0].intervals.size(), 1U);
  EXPECT_EQ(program.tasks[0].time[0].intervals[0].min.raw_int, 2);
  EXPECT_EQ(program.tasks[0].time[0].intervals[0].max.raw_int, 3);
  EXPECT_EQ(program.tasks[0].time[0].provenance.kind, dsl::ast::ProvenanceKind::Estimated);
  ASSERT_TRUE(program.tasks[0].time[0].provenance.by.has_value());
  EXPECT_EQ(program.tasks[0].time[0].provenance.by->value, "user spec sheet");

  const dsl::ast::TaskDecl& fusion = program.tasks[1];
  EXPECT_EQ(fusion.id, "fusion");
  EXPECT_EQ(fusion.effective_kind(), dsl::ast::TaskKind::Aperiodic);
  ASSERT_EQ(fusion.core.size(), 1U);
  EXPECT_EQ(fusion.core[0].kind, dsl::ast::CoreSpec::Kind::Set);
  EXPECT_EQ(fusion.core[0].set, (std::vector<std::int64_t>{1, 2, 3}));
  ASSERT_EQ(fusion.locks.size(), 1U);
  ASSERT_EQ(fusion.locks[0].locks.size(), 1U);
  EXPECT_EQ(fusion.locks[0].locks[0].name, "mutex1");
  ASSERT_EQ(fusion.time.size(), 1U);
  EXPECT_EQ(fusion.time[0].intervals.size(), 3U);
  EXPECT_EQ(fusion.time[0].provenance.kind, dsl::ast::ProvenanceKind::Measured);
  ASSERT_EQ(fusion.time[0].provenance.measure.runs.size(), 1U);
  EXPECT_EQ(fusion.time[0].provenance.measure.runs[0].value, 20);
  ASSERT_EQ(fusion.time[0].provenance.measure.timeout.size(), 1U);
  EXPECT_EQ(fusion.time[0].provenance.measure.timeout[0].value, 5);

  EXPECT_EQ(program.tasks[2].effective_kind(), dsl::ast::TaskKind::Interrupt);
  EXPECT_EQ(program.tasks[3].effective_kind(), dsl::ast::TaskKind::Aperiodic);
  ASSERT_EQ(program.tasks[4].core.size(), 1U);
  EXPECT_EQ(program.tasks[4].core[0].kind, dsl::ast::CoreSpec::Kind::Any);

  ASSERT_EQ(program.bindings.size(), 3U);
  EXPECT_EQ(program.bindings[0].kind, dsl::ast::BindingKind::Start);
  EXPECT_EQ(program.bindings[0].task.name, "sensor");
  EXPECT_EQ(program.bindings[1].task.name, "control");
  EXPECT_EQ(program.bindings[2].kind, dsl::ast::BindingKind::End);

  ASSERT_EQ(program.edges.size(), 2U);
  EXPECT_EQ(program.edges[0].sources.size(), 1U);
  EXPECT_EQ(program.edges[0].sources[0].name, "sensor");
  ASSERT_EQ(program.edges[0].targets.size(), 2U);
  EXPECT_EQ(program.edges[0].targets[0].name, "fusion");
  EXPECT_EQ(program.edges[0].targets[1].name, "log");
  ASSERT_EQ(program.edges[1].sources.size(), 2U);
  EXPECT_EQ(program.edges[1].sources[0].name, "fusion");
  ASSERT_EQ(program.edges[1].targets.size(), 1U);
  EXPECT_EQ(program.edges[1].targets[0].name, "display");

  EXPECT_TRUE(source_map.contains("task:fusion:time"));
  EXPECT_TRUE(source_map.contains("edge:sensor->fusion,log"));
  EXPECT_TRUE(source_map.contains("fork:__fork_sensor__fusion__log"));
  EXPECT_TRUE(source_map.contains("edge:fusion,log->display"));
  EXPECT_TRUE(source_map.contains("join:__join_fusion__log__display"));
  EXPECT_TRUE(source_map.contains("start:sensor"));
  EXPECT_TRUE(source_map.contains("end:display"));
  EXPECT_TRUE(source_map.contains("policy"));
  EXPECT_TRUE(source_map.contains("system:cpus"));

  const std::size_t start = source_map.find("edge:sensor->fusion,log")->byte_start;
  const std::size_t end = source_map.find("edge:sensor->fusion,log")->byte_end;
  EXPECT_EQ(std::string(kAppendixA).substr(start, end - start), "sensor -> {fusion, log}");
}

TEST(RtslParser, PanicModeReportsEveryIndependentSyntaxError) {
  const std::string source = R"(system s {
  cpus 2
  task a: periodic {
    period 10ms
    priority x
    core 0
    time [1, 2] estimated
  }
  task b: periodic {
    deadline
    core 0
    time [1, 2] estimated
  }
  task c: normal {
    priority 5x
    core 0
    time [1, 2] estimated
  }
  a b
  a -> b
  start 5
  b -> a
  end
}
)";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const dsl::ast::Program program = rtsl_test::parse_source(source, diagnostics, source_map);

  const auto errors = rtsl_test::diagnostics_with_code(diagnostics, "E-SYN-102");
  ASSERT_EQ(errors.size(), 6U);
  for (const ptpn::Diagnostic* error : errors) {
    EXPECT_NE(error->message.find("expected"), std::string::npos);
  }

  ASSERT_TRUE(errors[0]->location.has_value());
  EXPECT_EQ(errors[0]->location->line, 5);
  EXPECT_EQ(errors[0]->location->column, 14);
  ASSERT_TRUE(errors[1]->location.has_value());
  EXPECT_EQ(errors[1]->location->line, 11);
  ASSERT_TRUE(errors[2]->location.has_value());
  EXPECT_EQ(errors[2]->location->line, 15);
  EXPECT_EQ(errors[2]->location->column, 14);
  ASSERT_TRUE(errors[3]->location.has_value());
  EXPECT_EQ(errors[3]->location->line, 19);
  EXPECT_EQ(errors[3]->location->column, 5);
  ASSERT_TRUE(errors[4]->location.has_value());
  EXPECT_EQ(errors[4]->location->line, 21);
  EXPECT_EQ(errors[4]->location->column, 9);
  ASSERT_TRUE(errors[5]->location.has_value());
  EXPECT_EQ(errors[5]->location->line, 24);

  // The two well-formed edges after the broken one are still parsed.
  EXPECT_EQ(program.edges.size(), 2U);
  EXPECT_EQ(program.tasks.size(), 3U);
}

TEST(RtslParser, BrokenEdgeDoesNotSwallowFollowingEdges) {
  const std::string source = R"(system s {
  cpus 2
  task a: normal { time [1, 2] estimated }
  task b: normal { time [1, 2] estimated }
  a b
  a -> b
  b -> a
}
)";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const dsl::ast::Program program = rtsl_test::parse_source(source, diagnostics, source_map);

  ASSERT_EQ(rtsl_test::count_code(diagnostics, "E-SYN-102"), 1U);
  ASSERT_EQ(program.edges.size(), 2U);
  EXPECT_EQ(program.edges[0].sources[0].name, "a");
  EXPECT_EQ(program.edges[1].sources[0].name, "b");
}

TEST(RtslParser, ClosingBraceInsideAnEdgeSetIsNotASyncPoint) {
  const std::string source = R"(system s {
  cpus 2
  task a: normal { time [1, 2] estimated }
  task b: normal { time [1, 2] estimated }
  {a, b, } -> a
  a -> b
}
)";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const dsl::ast::Program program = rtsl_test::parse_source(source, diagnostics, source_map);

  const auto errors = rtsl_test::diagnostics_with_code(diagnostics, "E-SYN-102");
  ASSERT_EQ(errors.size(), 1U);
  ASSERT_TRUE(errors[0]->location.has_value());
  EXPECT_EQ(errors[0]->location->line, 5);
  EXPECT_EQ(errors[0]->location->column, 10);
  ASSERT_EQ(program.edges.size(), 1U);
  EXPECT_EQ(program.edges[0].sources[0].name, "a");
  EXPECT_EQ(program.edges[0].targets[0].name, "b");
}

TEST(RtslParser, FailedTaskIsTaintedAndSuppressesValidationCascades) {
  const std::string source = R"(system demo {
  cpus 2
  task a: periodic {
    period oops
    core 0
    time [1, 2] estimated
  }
}
)";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-SYN-102"), 1U);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-206"), 0U);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-211"), 0U);
  std::size_t validation_entries = 0;
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    if (diagnostic.category == "validation") {
      ++validation_entries;
    }
  }
  EXPECT_EQ(validation_entries, 0U);
}

TEST(RtslParser, DiagnosticCapAndTruncation) {
  std::string body = "system s {\n  cpus 2\n";
  for (int index = 0; index < 12; ++index) {
    body += "  a b\n";
  }
  body += "}\n";

  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::parse_source(body, diagnostics, source_map);

  EXPECT_EQ(diagnostics.size(), ptpn::Diagnostics::kMaxDiagnostics);
  EXPECT_TRUE(diagnostics.truncated());
}

TEST(RtslParser, MissingSystemBlockTaintsSystemAttributes) {
  const std::string source = R"(system demo
  task a: normal { time [1, 2] estimated }
)";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-SYN-102"), 1U);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-217"), 0U);
}

TEST(RtslParser, TrailingContentAfterTheSystemBlockIsASyntaxError) {
  const std::string source = R"(system demo {
  cpus 2
  task a: normal { time [1, 2] estimated }
}

this is not valid RTSL
)";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-SYN-102");
  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches[0]->location->line, 6);
  EXPECT_NE(matches[0]->message.find("end of file after the system block"), std::string::npos);
}

TEST(RtslParser, TimeSourceMapSpanCoversTheWholeAttribute) {
  const std::string source = R"(system s {
  cpus 2
  task fusion: aperiodic {
    core 0
    time measured by llm { runs 4, timeout 2, artifact "abc" }
  }
}
)";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const dsl::ast::Program program = rtsl_test::parse_source(source, diagnostics, source_map);
  ASSERT_TRUE(diagnostics.empty());
  ASSERT_EQ(program.tasks.size(), 1U);
  ASSERT_EQ(program.tasks[0].time.size(), 1U);

  const dsl::Span span = source_map.span_for("task:fusion:time").value();
  EXPECT_EQ(source.substr(span.byte_start, span.length()),
            "time measured by llm { runs 4, timeout 2, artifact \"abc\" }");
  EXPECT_EQ(span.line, 5);
  EXPECT_EQ(span.column, 5);
  EXPECT_EQ(span.byte_start, source.find("time measured"));
  EXPECT_EQ(span.byte_end,
            source.find("time measured") + std::string("time measured by llm { runs 4, timeout 2, "
                                                       "artifact \"abc\" }")
                                               .size());
}

}  // namespace
