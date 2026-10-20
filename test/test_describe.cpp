#include <cstddef>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app/describe.h"
#include "cli_test_helpers.h"
#include "common/diagnostics.h"
#include "dsl/ast.h"
#include "dsl/lower.h"
#include "dsl/source_map.h"
#include "dsl/validate.h"
#include "model/tdg.h"
#include "test_rtsl_helpers.h"

namespace {

using namespace ptpn_test;

// Runs the same pipeline the command uses: parse -> validate -> lower ->
// describe.
std::string describe(const std::string& source, const app::DescribeOptions& options = {}) {
  ptpn::Diagnostics diagnostics;
  dsl::SourceMap source_map;
  dsl::ast::Program program = rtsl_test::parse_source(source, diagnostics, source_map);
  dsl::ValidationOptions validation;
  validation.mode = dsl::ValidationOptions::Mode::Check;
  dsl::validate(program, source_map, diagnostics, validation);
  std::optional<tdg::TDG> lowered = dsl::lower(program, source_map, diagnostics, validation.mode);
  if (!lowered.has_value()) {
    return "";
  }
  return app::build_model_description(lowered.value(), &program, diagnostics, options);
}

}  // namespace

TEST(DescribeTest, ReportsEffectiveSemantics) {
  const std::string source = R"(system demo {
  cpus 2
  cores_per_cpu 1
  time_unit ms
  policy rm
  task a: periodic { period 10ms deadline 5ms core 0 time [1, 1] estimated }
  task b: aperiodic { core 1 locks mutex1 time [ [1, 1], [2, 2], [1, 1] ] estimated }
  task c: normal { core 1 time [1, 1] estimated }
  start a
  start b
  b -> c
  {b, c} -> c
}
)";
  const std::string text = describe(source);
  ASSERT_FALSE(text.empty());

  // Policy resolution, deadline semantics, segments with lock nesting and kinds.
  EXPECT_NE(text.find("policy rm"), std::string::npos);
  EXPECT_NE(text.find("the rm policy computes 1 during lowering"), std::string::npos);
  EXPECT_NE(text.find("deadline 5 (relative to release)"), std::string::npos);
  EXPECT_NE(text.find("pre-critical [1, 1] | mutex1 critical section [2, 2] | post-critical [1, 1]"),
            std::string::npos);
  EXPECT_NE(text.find("aperiodic (one-shot; same semantics as normal)"), std::string::npos);
  EXPECT_NE(text.find("periodic, core 0"), std::string::npos);

  // The first periodic release is at P, and `start` adds t=0.
  EXPECT_NE(text.find("first release at 10; the start binding adds t=0"), std::string::npos);

  // Lock usage and the validation notes that flag the traps.
  EXPECT_NE(text.find("mutex1: used by b"), std::string::npos);
  EXPECT_NE(text.find("E-VAL-223"), std::string::npos);  // b and c share core 1 + priority 100
}

TEST(DescribeTest, AlternativeEdgeStatementsAreDescribedAsChoices) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { core 0 time [1, 1] estimated }\n"
      "  task b: normal { core 0 time [1, 1] estimated }\n"
      "  task c: normal { core 0 time [1, 1] estimated }\n"
      "  start a\n"
      "  a -> b | c\n"
      "  b | c -> b");
  const std::string text = describe(source);
  ASSERT_FALSE(text.empty());
  EXPECT_NE(text.find("choice: a -> b | c"), std::string::npos);
  EXPECT_NE(text.find("only one target is released per completion"), std::string::npos);
  EXPECT_NE(text.find("choice: b | c -> b"), std::string::npos);
  EXPECT_NE(text.find("released by whichever source completes first"), std::string::npos);
  EXPECT_EQ(text.find("E-VAL-225"), std::string::npos);
}

TEST(DescribeTest, ExplicitForkAndJoinStaySilent) {
  const std::string source = rtsl_test::system_block(
      "  task a: normal { core 0 time [1, 1] estimated }\n"
      "  task b: normal { core 0 time [1, 1] estimated }\n"
      "  task c: normal { core 0 time [1, 1] estimated }\n"
      "  start a\n"
      "  a -> {b, c}\n"
      "  {b, c} -> c");
  const std::string text = describe(source);
  ASSERT_FALSE(text.empty());
  EXPECT_NE(text.find("AND fork (__fork_a__b__c)"), std::string::npos);
  EXPECT_NE(text.find("AND join (__join_b__c__c)"), std::string::npos);
  EXPECT_EQ(text.find("choice:"), std::string::npos);
}

TEST(DescribeCliTest, CommandPrintsAndEmbedsTheDescription) {
  const std::string path = write_temp_file(
      "describe_", ".rtsl",
      rtsl_test::system_block("  task a: periodic { period 10ms core 0 time [1, 1] "
                              "estimated }\n  start a"));

  const CliResult plain = run_cli({"describe", "-f", path});
  ASSERT_EQ(plain.exit_code, 0) << plain.stderr_text;
  EXPECT_NE(plain.stdout_text.find("model: 4 core(s)"), std::string::npos);
  EXPECT_NE(plain.stdout_text.find("a: periodic, core 0, priority 100"), std::string::npos);

  const CliResult envelope_result = run_cli({"describe", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(envelope_result.exit_code, 0) << envelope_result.stderr_text;
  const nlohmann::json envelope = parse_envelope(envelope_result.stdout_text);
  ASSERT_TRUE(envelope.at("result").contains("description"));
  EXPECT_NE(envelope.at("result").at("description").get<std::string>().find("a: periodic"),
            std::string::npos);
}

TEST(DescribeCliTest, PendingMeasurementIsReportedInsteadOfASemanticGuess) {
  const std::string path = write_temp_file(
      "describe_", ".rtsl",
      rtsl_test::system_block(
          "  task fusion: aperiodic { core 0 locks mutex1 time measured by llm { runs 3 } }\n"));
  const CliResult result = run_cli({"describe", "-f", path});
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_NE(result.stdout_text.find("pending measurements"), std::string::npos);
  EXPECT_NE(result.stdout_text.find("fusion: needs 3 interval(s)"), std::string::npos);
}
