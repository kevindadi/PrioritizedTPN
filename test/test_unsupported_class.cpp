#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

#include "app/lowering.h"
#include "cli_test_helpers.h"
#include "common/diagnostics.h"
#include "model/petri.h"
#include "model/tdg.h"

namespace {

const char* const kEdfOverlap = R"(system overlap {
  cpus 1
  cores_per_cpu 1
  time_unit ms
  policy edf
  task a: periodic {
    period 2ms
    deadline 4ms
    core 0
    time [3, 3] estimated
  }
}
)";

const char* const kFifoOverlap = R"(system overlap {
  cpus 1
  cores_per_cpu 1
  time_unit ms
  policy fifo
  task a: periodic {
    period 2ms
    deadline 4ms
    core 0
    time [3, 3] estimated
  }
}
)";

const char* const kLlfOverlap = R"(system overlap {
  cpus 1
  cores_per_cpu 1
  time_unit ms
  policy llf
  task a: periodic {
    period 2ms
    deadline 4ms
    core 0
    time [3, 3] estimated
  }
}
)";

const char* const kEdfSupported = R"(system ok {
  cpus 1
  cores_per_cpu 1
  time_unit ms
  policy edf
  task a: periodic {
    period 10ms
    deadline 10ms
    core 0
    time [1, 1] estimated
  }
}
)";

const char* const kEdfSymmetryApplied = R"(system symmetric_started {
  cpus 2
  cores_per_cpu 1
  time_unit ms
  policy edf
  task a: normal { core 0 deadline 10ms time [1, 1] estimated }
  task b: normal { core 1 deadline 10ms time [1, 1] estimated }
  start a
  start b
}
)";

const char* const kEdfSymmetryNotApplied = R"(system symmetric_periodic {
  cpus 2
  cores_per_cpu 1
  time_unit ms
  policy edf
  task a: periodic { period 10ms deadline 10ms core 0 time [1, 1] estimated }
  task b: periodic { period 20ms deadline 20ms core 1 time [1, 1] estimated }
}
)";

void expect_unsupported(const ptpn_test::CliResult& result) {
  ASSERT_EQ(result.exit_code, 4) << result.stderr_text;
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("exit_code"), 4);
  const nlohmann::json* diagnostic = ptpn_test::find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->at("category"), "unsupported");
  EXPECT_EQ(diagnostic->at("severity"), "error");
}

}  // namespace

TEST(UnsupportedClass, EdfOverlapIsTypedUnsupported) {
  const std::string path = ptpn_test::write_temp_file("unsupported_", ".rtsl", kEdfOverlap);
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"rtsl", "-f", path, "--diagnostics", "json"});

  expect_unsupported(result);
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  const nlohmann::json* diagnostic = ptpn_test::find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->at("subject"), "task:a");
  ASSERT_TRUE(diagnostic->contains("location"));
  EXPECT_EQ(diagnostic->at("location").at("line"), 6);
}

TEST(UnsupportedClass, FifoOverlapIsTypedUnsupported) {
  const std::string path = ptpn_test::write_temp_file("unsupported_", ".rtsl", kFifoOverlap);
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"rtsl", "-f", path, "--diagnostics", "json"});

  expect_unsupported(result);
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  const nlohmann::json* diagnostic = ptpn_test::find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->at("subject"), "task:a");
}

TEST(UnsupportedClass, LlfOverlapIsTypedUnsupported) {
  const std::string path = ptpn_test::write_temp_file("unsupported_", ".rtsl", kLlfOverlap);
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"rtsl", "-f", path, "--diagnostics", "json"});

  expect_unsupported(result);
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  const nlohmann::json* diagnostic = ptpn_test::find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->at("subject"), "task:a");
}

TEST(UnsupportedClass, JobClockExtrapolationIsTypedUnsupported) {
  const std::string path = ptpn_test::write_temp_file("unsupported_", ".rtsl", kEdfSupported);
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"rtsl", "-f", path, "--extrapolation", "--diagnostics", "json"});

  expect_unsupported(result);
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  const nlohmann::json* diagnostic = ptpn_test::find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->at("subject"), "option:extrapolation");
}

TEST(UnsupportedClass, JobClockSymmetryAppliedIsTypedUnsupported) {
  const std::string path =
      ptpn_test::write_temp_file("unsupported_", ".rtsl", kEdfSymmetryApplied);
  const ptpn_test::CliResult result = ptpn_test::run_cli(
      {"rtsl", "-f", path, "--reduction", "symmetry", "--diagnostics", "json"});

  expect_unsupported(result);
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  const nlohmann::json* diagnostic = ptpn_test::find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->at("subject"), "option:symmetry");
}

TEST(UnsupportedClass, JobClockSymmetryNotAppliedKeepsExistingBehavior) {
  const std::string path =
      ptpn_test::write_temp_file("unsupported_", ".rtsl", kEdfSymmetryNotApplied);
  const ptpn_test::CliResult result = ptpn_test::run_cli(
      {"rtsl", "-f", path, "--reduction", "symmetry", "--diagnostics", "json"});

  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  EXPECT_TRUE(ptpn_test::diagnostics_with_code(envelope, "E-UNS-001").empty());
  EXPECT_NE(result.stderr_text.find("no verified task permutation applies"), std::string::npos);
}

TEST(UnsupportedClass, SupportedJobClockModelStillSucceeds) {
  const std::string path = ptpn_test::write_temp_file("unsupported_", ".rtsl", kEdfSupported);
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"rtsl", "-f", path, "--diagnostics", "json"});

  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  EXPECT_TRUE(envelope.at("ok").get<bool>());
  EXPECT_TRUE(ptpn_test::diagnostics_with_code(envelope, "E-UNS-001").empty());
}

TEST(UnsupportedClass, LegacyModeStillUsesExitCodeOne) {
  const std::string path = ptpn_test::write_temp_file("unsupported_", ".rtsl", kEdfOverlap);
  const ptpn_test::CliResult result = ptpn_test::run_cli({"rtsl", "-f", path});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.stdout_text.find("\"protocol\""), std::string::npos);
  const std::string combined = result.stdout_text + result.stderr_text;
  EXPECT_NE(combined.find("Unsupported model or option"), std::string::npos);
  EXPECT_EQ(combined.find("E-UNS-001"), std::string::npos);
}

TEST(UnsupportedClass, AllocationKeepsCandidateLevelRejection) {
  const std::string path = ptpn_test::write_temp_file("unsupported_", ".rtsl", kEdfOverlap);
  const std::string report_path = ptpn_test::test_artifact_name("unsupported_", ".alloc.json");
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"allocate", "-f", path, "-o", report_path, "--diagnostics", "json"});

  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  EXPECT_TRUE(envelope.at("ok").get<bool>());
  EXPECT_TRUE(ptpn_test::diagnostics_with_code(envelope, "E-UNS-001").empty());

  const nlohmann::json report =
      nlohmann::json::parse(ptpn_test::read_file(report_path), nullptr, false);
  ASSERT_FALSE(report.is_discarded());
  EXPECT_TRUE(report.at("best").is_null());
  EXPECT_EQ(report.at("search").at("feasible_count"), 0);
  EXPECT_TRUE(report.at("search").at("complete").get<bool>());
}

TEST(UnsupportedClass, InternalLoweringFailureStaysInternal) {
  tdg::TDG graph(1, 1);
  TaskNode task;
  task.name = "broken";
  task.core = 0;
  task.priority = 100;
  task.time.clear();  // no execution segments: vertex lowering throws runtime_error
  graph.all_task.push_back(task);
  graph.nodes_type.emplace("broken", task);
  graph.tasks_priority.emplace("broken", 100);
  graph.start_tasks.push_back({"broken", 1});

  petri::PTPN net;
  ptpn::Diagnostics diagnostics;
  EXPECT_EQ(app::build_ptpn_from_tdg(graph, net, &diagnostics), 1);
  bool saw_internal = false;
  bool saw_unsupported = false;
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    saw_internal = saw_internal || diagnostic.code == "E-INT-003";
    saw_unsupported = saw_unsupported || diagnostic.code == "E-UNS-001";
  }
  EXPECT_TRUE(saw_internal);
  EXPECT_FALSE(saw_unsupported);
}
