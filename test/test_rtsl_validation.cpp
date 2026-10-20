#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

#include "common/diagnostics.h"
#include "dsl/ast.h"
#include "dsl/source_map.h"
#include "dsl/validate.h"
#include "test_rtsl_helpers.h"

namespace {

std::set<std::string> subjects_for(const ptpn::Diagnostics& diagnostics, const std::string& code) {
  std::set<std::string> subjects;
  for (const ptpn::Diagnostic* diagnostic : rtsl_test::diagnostics_with_code(diagnostics, code)) {
    if (diagnostic->subject.has_value()) {
      subjects.insert(diagnostic->subject.value());
    }
  }
  return subjects;
}

TEST(RtslValidation, DuplicateTaskIdReportsBothDeclarations) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { time [1, 2] estimated }\n"
      "  task a: normal { time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-201");
  ASSERT_EQ(matches.size(), 1U);
  ASSERT_TRUE(matches[0]->subject.has_value());
  EXPECT_EQ(matches[0]->subject.value(), "task:a");
  ASSERT_TRUE(matches[0]->location.has_value());
  EXPECT_EQ(matches[0]->location->line, 4);
  EXPECT_NE(matches[0]->message.find("line 3"), std::string::npos);
}

TEST(RtslValidation, ReservedDoubleUnderscoreInTaskAndLockNames) {
  const std::string source = rtsl_test::system_block(
      "  task bad__task: normal { time [1, 2] estimated }\n"
      "  task lockuser: normal { locks mutex__x time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-202"), 2U);
  const std::set<std::string> subjects = subjects_for(diagnostics, "E-VAL-202");
  EXPECT_TRUE(subjects.count("task:bad__task") == 1U);
  EXPECT_TRUE(subjects.count("lock:mutex__x") == 1U);
}

TEST(RtslValidation, DuplicateEdgeReportsBothLocations) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { time [1, 2] estimated }\n"
      "  task b: normal { time [1, 2] estimated }\n"
      "  {a, b} -> a\n"
      "  {b, a} -> a");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-203");
  ASSERT_EQ(matches.size(), 2U);
  ASSERT_TRUE(matches[0]->location.has_value());
  ASSERT_TRUE(matches[1]->location.has_value());
  EXPECT_EQ(matches[0]->location->line, 5);
  EXPECT_EQ(matches[1]->location->line, 6);
  EXPECT_EQ(matches[0]->subject.value(), "edge:a,b->a");
  EXPECT_EQ(matches[1]->subject.value(), "edge:a,b->a");
}

TEST(RtslValidation, UnknownTasksInEdgesAndBindings) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { time [1, 2] estimated }\n"
      "  a -> ghost\n"
      "  start missing\n"
      "  end gone");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-204"), 3U);
  const std::set<std::string> subjects = subjects_for(diagnostics, "E-VAL-204");
  EXPECT_TRUE(subjects.count("edge:a->ghost") == 1U);
  EXPECT_TRUE(subjects.count("start:missing") == 1U);
  EXPECT_TRUE(subjects.count("end:gone") == 1U);
}

TEST(RtslValidation, KindAttributeMismatch) {
  const std::string source = rtsl_test::system_block(
      "  task p: periodic { period 10ms mit 5ms time [1, 2] estimated }\n"
      "  task ap: aperiodic { period 10ms time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-205"), 2U);
  const std::set<std::string> subjects = subjects_for(diagnostics, "E-VAL-205");
  EXPECT_TRUE(subjects.count("task:p:mit") == 1U);
  EXPECT_TRUE(subjects.count("task:ap:period") == 1U);
}

TEST(RtslValidation, NonPositivePeriodMitDeadline) {
  const std::string source = rtsl_test::system_block(
      "  task p: periodic { period 0 time [1, 2] estimated }\n"
      "  task sp: sporadic { mit 0 time [1, 2] estimated }\n"
      "  task d: normal { deadline 0 time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-206"), 1U);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-207"), 1U);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-208"), 1U);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-UNS-003"), 1U);
  EXPECT_EQ(subjects_for(diagnostics, "E-VAL-206").count("task:p:period"), 1U);
  EXPECT_EQ(subjects_for(diagnostics, "E-VAL-207").count("task:sp:mit"), 1U);
  EXPECT_EQ(subjects_for(diagnostics, "E-VAL-208").count("task:d:deadline"), 1U);
}

TEST(RtslValidation, CoreOutOfRange) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { core 5 time [1, 2] estimated }\n"
      "  task b: normal { core {0, 7} time [1, 2] estimated }",
      "  cpus 2\n");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-209"), 2U);
  EXPECT_EQ(subjects_for(diagnostics, "E-VAL-209").count("task:a:core"), 1U);
  EXPECT_EQ(subjects_for(diagnostics, "E-VAL-209").count("task:b:core"), 1U);
}

TEST(RtslValidation, EmptyAndDuplicateCoreSets) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { core {} time [1, 2] estimated }\n"
      "  task b: normal { core {1, 1} time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-210"), 2U);
}

TEST(RtslValidation, MissingRequiredAttributes) {
  const std::string source = rtsl_test::system_block(
      "  task n: normal { core 0 }\n"
      "  task p: periodic { core 0 time [1, 2] estimated }\n"
      "  task sp: sporadic { core 0 time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-211"), 3U);
  const std::set<std::string> subjects = subjects_for(diagnostics, "E-VAL-211");
  EXPECT_TRUE(subjects.count("task:n") == 1U);
  EXPECT_TRUE(subjects.count("task:p:period") == 1U);
  EXPECT_TRUE(subjects.count("task:sp:mit") == 1U);
}

TEST(RtslValidation, IntervalMinimumGreaterThanMaximum) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { time [5, 3] estimated }\n"
      "  task b: normal { time [ [1, 2], [4, 3] ] certified by \"tool\" }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-212"), 2U);
  EXPECT_EQ(subjects_for(diagnostics, "E-VAL-212").count("task:a:time"), 1U);
  EXPECT_EQ(subjects_for(diagnostics, "E-VAL-212").count("task:b:time"), 1U);
}

TEST(RtslValidation, EstimatedAndCertifiedRequireIntervals) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { time estimated }\n"
      "  task b: normal { time certified by \"tool\" }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-214"), 2U);
}

TEST(RtslValidation, UnknownPolicyIsRejected) {
  const std::string source = rtsl_test::system_block("", "  cpus 2\n  policy round_robin\n");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-215");
  ASSERT_EQ(matches.size(), 1U);
  ASSERT_TRUE(matches[0]->subject.has_value());
  EXPECT_EQ(matches[0]->subject.value(), "policy");
  EXPECT_NE(matches[0]->message.find("round_robin"), std::string::npos);
}

TEST(RtslValidation, MultiToMultiEdgesAreRejected) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { time [1, 2] estimated }\n"
      "  task b: normal { time [1, 2] estimated }\n"
      "  task c: normal { time [1, 2] estimated }\n"
      "  task d: normal { time [1, 2] estimated }\n"
      "  {a, b} -> {c, d}");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-216");
  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches[0]->subject.value(), "edge:a,b->c,d");
}

TEST(RtslValidation, SystemRequiredAttributesAndRanges) {
  {
    const std::string source = rtsl_test::system_block("", "");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-217"), 1U);
    EXPECT_EQ(subjects_for(diagnostics, "E-VAL-217").count("system:cpus"), 1U);
  }
  {
    const std::string source = rtsl_test::system_block("", "  cpus 0\n");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-217"), 1U);
  }
  {
    const std::string source = rtsl_test::system_block("", "  cpus 2\n  cores_per_cpu 0\n");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-217"), 1U);
    EXPECT_EQ(subjects_for(diagnostics, "E-VAL-217").count("system:cores_per_cpu"), 1U);
  }
}

TEST(RtslValidation, DuplicateAttributesAreReported) {
  const std::string source =
      "system demo {\n"
      "  cpus 2\n"
      "  cpus 3\n"
      "  policy fixed\n"
      "  policy rm\n"
      "  task a: normal { priority 1 priority 2 core 0 time [1, 2] estimated }\n"
      "  task b: normal { time measured by llm { runs 1, runs 2 } }\n"
      "}\n";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-219"), 4U);
  const std::set<std::string> subjects = subjects_for(diagnostics, "E-VAL-219");
  EXPECT_TRUE(subjects.count("system:cpus") == 1U);
  EXPECT_TRUE(subjects.count("policy") == 1U);
  EXPECT_TRUE(subjects.count("task:a:priority") == 1U);
  EXPECT_TRUE(subjects.count("task:b:time") == 1U);
}

TEST(RtslValidation, MeasurementConfigurationMustBePositive) {
  const std::string source =
      rtsl_test::system_block("  task a: normal { time measured by llm { runs 0, timeout 0 } }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-220"), 2U);
}

TEST(RtslValidation, OutOfRangeNumbersAreRejectedBeforeLowering) {
  {
    // 3000000 s = 3e9 ms: overflows the signed 32-bit tick range.
    const std::string source = rtsl_test::system_block(
        "  task a: periodic { period 3000000s deadline 3000000s core 0 time [1, 1] estimated }");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);

    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-221"), 2U);
    const std::set<std::string> subjects = subjects_for(diagnostics, "E-VAL-221");
    EXPECT_TRUE(subjects.count("task:a:period") == 1U);
    EXPECT_TRUE(subjects.count("task:a:deadline") == 1U);
  }
  {
    // 4294967297 wraps to 1 in a 32-bit priority.
    const std::string source =
        rtsl_test::system_block("  task a: normal { priority 4294967297 time [1, 1] estimated }");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);

    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-221"), 1U);
    EXPECT_EQ(subjects_for(diagnostics, "E-VAL-221").count("task:a:priority"), 1U);
  }
  {
    // Interval endpoints are checked too; INT_MAX is the +infinity sentinel.
    const std::string source =
        rtsl_test::system_block("  task a: normal { time [1, 2147483647] estimated }");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);

    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-221"), 1U);
    EXPECT_EQ(subjects_for(diagnostics, "E-VAL-221").count("task:a:time"), 1U);
  }
  {
    // The largest representable finite tick is accepted.
    const std::string source = rtsl_test::system_block(
        "  task a: periodic { period 2147483646ms core 0 time [1, 1] estimated }");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);

    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-221"), 0U);
  }
}

TEST(RtslValidation, InvalidLockPrefixReusesJsonSemantics) {
  const std::string source =
      rtsl_test::system_block("  task a: normal { locks foo time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-014");
  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches[0]->subject.value(), "lock:foo");
  EXPECT_NE(matches[0]->message.find("must start with 'mutex' or 'spin'"), std::string::npos);
}

TEST(RtslValidation, SporadicIsUnsupportedWithoutCertificationClaim) {
  const std::string source =
      rtsl_test::system_block("  task sp: sporadic { mit 10 time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-UNS-003");
  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches[0]->category, "unsupported");
  EXPECT_EQ(matches[0]->severity, "error");
  ASSERT_TRUE(matches[0]->fix.has_value());
  const std::string& fix = matches[0]->fix.value();
  EXPECT_NE(fix.find("period 10ms"), std::string::npos);
  EXPECT_NE(fix.find("does not certify"), std::string::npos);
  EXPECT_EQ(fix.find("period = mit"), std::string::npos);
  EXPECT_EQ(fix.find("conservative WCRT"), std::string::npos);
  EXPECT_EQ(diagnostics.exit_code(), 4);
}

TEST(RtslValidation, MicrosecondUnitsAreUnsupportedEverywhere) {
  {
    const std::string source =
        rtsl_test::system_block("  task a: normal { time [1us, 2us] estimated }");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-UNS-004"), 2U);
    EXPECT_EQ(diagnostics.exit_code(), 4);
  }
  {
    const std::string source = rtsl_test::system_block("", "  cpus 2\n  time_unit us\n");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-UNS-004");
    ASSERT_EQ(matches.size(), 1U);
    EXPECT_EQ(matches[0]->subject.value(), "system:time_unit");
  }
}

TEST(RtslValidation, UnknownUnitSuffixIsSyntax) {
  const std::string source =
      rtsl_test::system_block("  task a: normal { time [1ns, 2ns] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-SYN-101"), 2U);
  EXPECT_EQ(diagnostics.exit_code(), 2);
}

TEST(RtslValidation, UnknownTimeUnitIsSyntax) {
  const std::string source = rtsl_test::system_block("", "  cpus 2\n  time_unit ns\n");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-SYN-101");
  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches[0]->subject.value(), "system:time_unit");
}

TEST(RtslValidation, CheckModeHintsDoNotAffectTheExitCode) {
  const std::string source = rtsl_test::system_block(
      "  task i: interrupt { core 0 time [1, 2] estimated }\n"
      "  task l: normal { core any time [1, 2] estimated }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  // E-UNS-005 (interrupt), E-VAL-218 (core any) and E-VAL-223 (both tasks are
  // on core 0 with the default priority 100).
  ASSERT_EQ(diagnostics.size(), 3U);
  const auto interrupt = rtsl_test::diagnostics_with_code(diagnostics, "E-UNS-005");
  ASSERT_EQ(interrupt.size(), 1U);
  EXPECT_EQ(interrupt[0]->severity, "hint");
  const auto allocate = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-218");
  ASSERT_EQ(allocate.size(), 1U);
  EXPECT_EQ(allocate[0]->severity, "hint");
  EXPECT_NE(allocate[0]->message.find("ptpn allocate"), std::string::npos);
  const auto tie_hint = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-223");
  ASSERT_EQ(tie_hint.size(), 1U);
  EXPECT_EQ(tie_hint[0]->severity, "hint");
  EXPECT_EQ(diagnostics.exit_code(), 0);
}

TEST(RtslValidation, CoreSetIsHintedInCheckAndWarnedInRtsl) {
  const std::string source =
      rtsl_test::system_block("  task a: normal { core {2, 0} time [1, 2] estimated }");

  // `check` hints: the set is a valid allocation domain for `ptpn allocate`.
  {
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-218");
    ASSERT_EQ(matches.size(), 1U);
    EXPECT_EQ(matches[0]->severity, "hint");
    EXPECT_NE(matches[0]->message.find("{0, 2}"), std::string::npos);
    EXPECT_NE(matches[0]->message.find("core 0"), std::string::npos);
    EXPECT_EQ(diagnostics.exit_code(), 0);
  }

  // `rtsl` warns (analysis would silently pin the task to core 0); the model
  // still lowers, so the exit code stays 0 without other errors.
  {
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    dsl::ValidationOptions options;
    options.mode = dsl::ValidationOptions::Mode::Rtsl;
    rtsl_test::check_source(source, diagnostics, source_map, options);
    const auto matches = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-218");
    ASSERT_EQ(matches.size(), 1U);
    EXPECT_EQ(matches[0]->severity, "warning");
    EXPECT_EQ(diagnostics.exit_code(), 0);
  }

  // A single-element set is exactly `core N`; no diagnostic.
  {
    const std::string single =
        rtsl_test::system_block("  task a: normal { core {1} time [1, 2] estimated }");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(single, diagnostics, source_map);
    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-218"), 0U);
  }
}

TEST(RtslValidation, ScheduleHintsAreNonBlocking) {
  // deadline > period (warning), same core + priority (hint), and utilisation
  // above 1 (warning) all leave the exit code at 0.
  const std::string source =
      "system demo {\n"
      "  cpus 1\n"
      "  cores_per_cpu 1\n"
      "  time_unit ms\n"
      "  task a: periodic { period 10ms deadline 20ms core 0 time [6, 7] estimated }\n"
      "  task b: periodic { period 10ms deadline 10ms core 0 time [6, 7] estimated }\n"
      "}\n";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto deadline_hint = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-222");
  ASSERT_EQ(deadline_hint.size(), 1U);
  EXPECT_EQ(deadline_hint[0]->severity, "warning");
  EXPECT_NE(deadline_hint[0]->message.find("exceeds the period"), std::string::npos);

  const auto tie_hint = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-223");
  ASSERT_EQ(tie_hint.size(), 1U);
  EXPECT_EQ(tie_hint[0]->severity, "hint");

  const auto utilisation = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-224");
  ASSERT_EQ(utilisation.size(), 1U);
  EXPECT_EQ(utilisation[0]->severity, "warning");
  EXPECT_EQ(utilisation[0]->subject.value(), "system:core0");
  EXPECT_NE(utilisation[0]->message.find("best-case execution times"), std::string::npos);
  EXPECT_EQ(diagnostics.exit_code(), 0);
}

TEST(RtslValidation, SeparateEdgeStatementsAreRejected) {
  // Two statements sharing a source or target used to lower to the
  // token-consuming alternative semantics silently; that is now an error, so
  // every fork/join/choice is explicit.
  const std::string source = rtsl_test::system_block(
      "  task a: normal { core 0 time [1, 1] estimated }\n"
      "  task b: normal { core 0 time [1, 1] estimated }\n"
      "  task c: normal { core 0 time [1, 1] estimated }\n"
      "  start a\n"
      "  a -> b\n"
      "  a -> c\n"
      "  b -> c");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);

  const auto errors = rtsl_test::diagnostics_with_code(diagnostics, "E-VAL-225");
  ASSERT_EQ(errors.size(), 2U);
  for (const ptpn::Diagnostic* error : errors) {
    EXPECT_EQ(error->severity, "error");
    EXPECT_NE(error->message.find("separate"), std::string::npos);
    ASSERT_TRUE(error->fix.has_value());
    EXPECT_NE(error->fix->find("|"), std::string::npos);
  }
  const std::set<std::string> subjects = subjects_for(diagnostics, "E-VAL-225");
  EXPECT_TRUE(subjects.count("task:a") == 1U);  // two outgoing statements
  EXPECT_TRUE(subjects.count("task:c") == 1U);  // two incoming statements
  EXPECT_EQ(diagnostics.exit_code(), 3);

  // The same endpoints written as one explicit alternative statement are fine.
  const std::string explicit_form = rtsl_test::system_block(
      "  task a: normal { core 0 time [1, 1] estimated }\n"
      "  task b: normal { core 0 time [1, 1] estimated }\n"
      "  task c: normal { core 0 time [1, 1] estimated }\n"
      "  task d: normal { core 0 time [1, 1] estimated }\n"
      "  start a\n"
      "  a -> b | c\n"
      "  b | c -> d");
  ptpn::Diagnostics explicit_diagnostics;
  dsl::SourceMap explicit_map;
  const dsl::ast::Program program =
      rtsl_test::check_source(explicit_form, explicit_diagnostics, explicit_map);
  EXPECT_EQ(rtsl_test::count_code(explicit_diagnostics, "E-VAL-225"), 0U);
  EXPECT_EQ(explicit_diagnostics.exit_code(), 0);
  ASSERT_EQ(program.edges.size(), 2U);
  EXPECT_TRUE(program.edges[0].alternative);
  EXPECT_TRUE(program.edges[1].alternative);
  EXPECT_EQ(program.edges[0].target_names(), (std::vector<std::string>{"b", "c"}));
  EXPECT_EQ(program.edges[1].source_names(), (std::vector<std::string>{"b", "c"}));

  // The braced AND form stays silent too, and mixing the two is a syntax error.
  const std::string and_form = rtsl_test::system_block(
      "  task a: normal { core 0 time [1, 1] estimated }\n"
      "  task b: normal { core 0 time [1, 1] estimated }\n"
      "  task c: normal { core 0 time [1, 1] estimated }\n"
      "  start a\n"
      "  a -> {b, c}\n"
      "  {b, c} -> c");
  ptpn::Diagnostics and_diagnostics;
  dsl::SourceMap and_map;
  rtsl_test::check_source(and_form, and_diagnostics, and_map);
  EXPECT_EQ(rtsl_test::count_code(and_diagnostics, "E-VAL-225"), 0U);

  const std::string mixed = rtsl_test::system_block(
      "  task a: normal { core 0 time [1, 1] estimated }\n"
      "  task b: normal { core 0 time [1, 1] estimated }\n"
      "  task c: normal { core 0 time [1, 1] estimated }\n"
      "  start a\n"
      "  a -> {b} | c");
  ptpn::Diagnostics mixed_diagnostics;
  dsl::SourceMap mixed_map;
  rtsl_test::check_source(mixed, mixed_diagnostics, mixed_map);
  EXPECT_EQ(rtsl_test::count_code(mixed_diagnostics, "E-SYN-102"), 1U);
}

TEST(RtslValidation, MeasuredMissingIntervalsAreAcceptedByCheck) {
  const std::string source =
      rtsl_test::system_block("  task a: normal { time measured by llm { runs 20 } }");
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  const dsl::ast::Program program = rtsl_test::check_source(source, diagnostics, source_map);

  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-213"), 0U);
  EXPECT_TRUE(diagnostics.empty());
  EXPECT_EQ(diagnostics.exit_code(), 0);

  const nlohmann::json pending = dsl::collect_pending_measurements(program, source_map);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending[0].at("task"), "a");
  EXPECT_EQ(pending[0].at("expected_segments"), 1);
  EXPECT_EQ(pending[0].at("runs"), 20);
  EXPECT_EQ(pending[0].at("timeout"), 5);
  EXPECT_TRUE(pending[0].contains("inject"));
}

TEST(RtslValidation, UnitNormalizationUsesTimeUnitForBareIntegers) {
  {
    const std::string source = rtsl_test::system_block("  task a: normal { time [2, 1] estimated }",
                                                       "  cpus 2\n  time_unit s\n");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-212"), 1U);
  }
  {
    const std::string source = rtsl_test::system_block(
        "  task a: normal { time [2, 3000ms] estimated }", "  cpus 2\n  time_unit s\n");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-212"), 0U);
  }
  {
    const std::string source = rtsl_test::system_block(
        "  task a: normal { time [1s, 500ms] estimated }", "  cpus 2\n  time_unit s\n");
    ptpn::Diagnostics diagnostics;
    dsl::SourceMap source_map;
    rtsl_test::check_source(source, diagnostics, source_map);
    EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-VAL-212"), 1U);
  }
}

TEST(RtslValidation, SourceMapBindsEverySubjectFamily) {
  const std::string source = R"(system demo {
  cpus 2
  cores_per_cpu 1
  time_unit ms
  policy rm
  task a: periodic {
    period 10ms
    deadline 5ms
    priority 5
    core 0
    locks mutex1
    time [1, 2] estimated
  }
  task b: normal { time [1, 2] estimated }
  start a
  end b
  a -> {b}
  {a, b} -> b
}
)";
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  rtsl_test::check_source(source, diagnostics, source_map);
  // The model deliberately declares two outgoing edge statements from `a` and
  // two incoming into `b` (to bind both the edge and the join subject); that
  // shared-endpoint form is now E-VAL-225, and the source map must still be
  // fully populated for an invalid model.
  EXPECT_EQ(diagnostics.exit_code(), 3);
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    EXPECT_EQ(diagnostic.code, "E-VAL-225");
    EXPECT_EQ(diagnostic.severity, "error");
  }

  const std::vector<std::string> keys = {
      "task:a",           "task:a:core",     "task:a:time",
      "task:a:period",    "task:a:deadline", "task:a:priority",
      "lock:mutex1",      "edge:a->b",       "join:__join_a__b__b",
      "policy",           "system:cpus",     "system:cores_per_cpu",
      "system:time_unit", "start:a",         "end:b",
  };
  for (const std::string& key : keys) {
    EXPECT_TRUE(source_map.contains(key)) << key;
  }

  const std::optional<ptpn::SourceLocation> time_location = source_map.location_for("task:a:time");
  ASSERT_TRUE(time_location.has_value());
  EXPECT_EQ(time_location->file, "test.rtsl");
  EXPECT_EQ(time_location->line, 12);
  EXPECT_GT(time_location->span, 0);
}

}  // namespace
