#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "cli_test_helpers.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#  include <sys/wait.h>
#endif

namespace {

#ifndef PTPN_CLI_PATH
#  define PTPN_CLI_PATH "ptpn"
#endif

using namespace ptpn_test;

std::string test_artifact_name(const std::string& suffix) {
  return ptpn_test::test_artifact_name("pipe_", suffix);
}

std::string write_temp_file(const std::string& suffix, const std::string& contents) {
  return ptpn_test::write_temp_file("pipe_", suffix, contents);
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

const char* const kMeasuredPending = R"(system s {
  cpus 1
  task a: normal {
    core 0
    time measured by llm { runs 3, timeout 2 }
  }
  start a
}
)";

const char* const kCoreAny = R"(system s {
  cpus 2
  task a: normal {
    core any
    time [1, 1] estimated
  }
  start a
}
)";

}  // namespace

TEST(RtslPipelineCli, ExportTdgRoundTripsWithIdenticalMetrics) {
  const std::string source_path = write_temp_file(".rtsl", kAppendix);
  const std::string tdg_path = test_artifact_name(".tdg.json");
  const std::string rtsl_metrics_path = test_artifact_name(".rtsl.metrics.json");
  const std::string tdg_metrics_path = test_artifact_name(".tdg.metrics.json");

  const CliResult rtsl = run_cli({"rtsl", "-f", source_path, "--export-tdg", tdg_path,
                                  "--export-metrics", rtsl_metrics_path, "--diagnostics", "json"});
  ASSERT_EQ(rtsl.exit_code, 0) << rtsl.stderr_text;
  const nlohmann::json envelope = parse_envelope(rtsl.stdout_text);
  EXPECT_EQ(envelope.at("ok"), true);
  EXPECT_EQ(envelope.at("exit_code"), 0);
  EXPECT_EQ(envelope.at("result").at("analysis").at("exact"), true);
  EXPECT_EQ(envelope.at("result").at("analysis").at("states"), 82);
  EXPECT_FALSE(has_diagnostic(envelope, "E-UNS-001"));

  const std::string lowered = read_file(tdg_path);
  ASSERT_FALSE(lowered.empty());
  const nlohmann::json lowered_json = nlohmann::json::parse(lowered, nullptr, false);
  ASSERT_FALSE(lowered_json.is_discarded());
  bool saw_fork = false;
  for (const auto& node : lowered_json.at("nodes")) {
    if (node.value("id", "") == "__fork_sensor__fusion__log") {
      saw_fork = true;
      EXPECT_EQ(node.value("type", ""), "fork");
    }
  }
  EXPECT_TRUE(saw_fork);

  const CliResult tdg = run_cli(
      {"tdg", "-f", tdg_path, "--export-metrics", tdg_metrics_path, "--diagnostics", "json"});
  ASSERT_EQ(tdg.exit_code, 0) << tdg.stderr_text;
  EXPECT_EQ(read_file(rtsl_metrics_path), read_file(tdg_metrics_path));
}

TEST(RtslPipelineCli, MeasuredWithoutIntervalsIsRejectedByRtslAndAllocate) {
  const std::string path = write_temp_file(".rtsl", kMeasuredPending);

  const CliResult check = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(check.exit_code, 0) << check.stderr_text;
  EXPECT_FALSE(has_diagnostic(parse_envelope(check.stdout_text), "E-VAL-213"));

  const CliResult rtsl = run_cli({"rtsl", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(rtsl.exit_code, 3) << rtsl.stderr_text;
  const nlohmann::json rtsl_envelope = parse_envelope(rtsl.stdout_text);
  const nlohmann::json* pending = find_diagnostic(rtsl_envelope, "E-VAL-213");
  ASSERT_NE(pending, nullptr);
  EXPECT_EQ(pending->at("severity"), "error");
  EXPECT_EQ(pending->at("fix"), "fill the time interval in the RTSL source first");
  ASSERT_TRUE(pending->contains("location"));
  EXPECT_EQ(pending->at("location").at("file"), path);
  EXPECT_EQ(pending->at("location").at("line"), 5);

  const std::string report_path = test_artifact_name(".allocation.json");
  const CliResult allocate =
      run_cli({"allocate", "-f", path, "-o", report_path, "--diagnostics", "json"});
  ASSERT_EQ(allocate.exit_code, 3) << allocate.stderr_text;
  const nlohmann::json allocate_envelope = parse_envelope(allocate.stdout_text);
  EXPECT_EQ(allocate_envelope.at("ok"), false);
  EXPECT_NE(find_diagnostic(allocate_envelope, "E-VAL-213"), nullptr);

  const CliResult legacy = run_cli({"rtsl", "-f", path});
  EXPECT_EQ(legacy.exit_code, 1);
  EXPECT_NE(legacy.stderr_text.find("E-VAL-213"), std::string::npos);
  EXPECT_NE(legacy.stderr_text.find("fill the time interval in the RTSL source first"),
            std::string::npos);
}

TEST(RtslPipelineCli, CoreAnyIsRejectedByRtslAndExpandedByAllocate) {
  const std::string path = write_temp_file(".rtsl", kCoreAny);

  const CliResult rtsl = run_cli({"rtsl", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(rtsl.exit_code, 3) << rtsl.stderr_text;
  const nlohmann::json envelope = parse_envelope(rtsl.stdout_text);
  const nlohmann::json* any = find_diagnostic(envelope, "E-VAL-218");
  ASSERT_NE(any, nullptr);
  EXPECT_EQ(any->at("severity"), "error");
  EXPECT_EQ(any->at("subject"), "task:a:core");
  EXPECT_EQ(any->at("fix"), "use `ptpn allocate` to expand the core domain before analysis");

  const std::string report_path = test_artifact_name(".allocation.json");
  const CliResult allocate =
      run_cli({"allocate", "-f", path, "-o", report_path, "--diagnostics", "json"});
  ASSERT_EQ(allocate.exit_code, 0) << allocate.stderr_text;
  const nlohmann::json allocate_envelope = parse_envelope(allocate.stdout_text);
  EXPECT_EQ(allocate_envelope.at("ok"), true);
  EXPECT_FALSE(has_diagnostic(allocate_envelope, "E-VAL-218"));

  const nlohmann::json report = nlohmann::json::parse(read_file(report_path), nullptr, false);
  ASSERT_FALSE(report.is_discarded());
  EXPECT_EQ(report.at("search").at("domain_size"), 2);
  EXPECT_EQ(report.at("search").at("evaluated"), 2);
  EXPECT_EQ(report.at("search").at("feasible_count"), 2);
  ASSERT_TRUE(report.contains("best"));
  EXPECT_TRUE(report.at("best").at("feasible"));
}

TEST(RtslPipelineCli, UnsupportedClassReportsEvalUns001WithTaskLocation) {
  const std::string source = R"(system s {
  cpus 2
  policy edf
  task a: normal {
    core 0
    locks mutex1
    time [ [1, 1], [2, 2], [1, 1] ] estimated
  }
  start a
}
)";
  const std::string path = write_temp_file(".rtsl", source);
  const CliResult result = run_cli({"rtsl", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 4) << result.stderr_text;
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  const nlohmann::json* unsupported = find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(unsupported, nullptr);
  EXPECT_EQ(unsupported->at("severity"), "error");
  EXPECT_EQ(unsupported->at("subject"), "task:a");
  EXPECT_EQ(unsupported->at("fix"), "simplify the model or choose a supported scheduling policy");
  ASSERT_TRUE(unsupported->contains("location"));
  EXPECT_EQ(unsupported->at("location").at("file"), path);
  EXPECT_EQ(unsupported->at("location").at("line"), 4);
}

TEST(RtslPipelineCli, UnsupportedForkBindsToItsEdgeDeclaration) {
  const std::string source = R"(system s {
  cpus 2
  policy edf
  task a: normal { core 0 deadline 10 time [1, 1] estimated }
  task b: periodic { core 0 period 10ms deadline 10 time [1, 1] estimated }
  task c: periodic { core 0 period 10ms deadline 10 time [1, 1] estimated }
  start a
  a -> {b, c}
}
)";
  const std::string path = write_temp_file(".rtsl", source);
  const CliResult result = run_cli({"rtsl", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 4) << result.stderr_text;
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  const nlohmann::json* unsupported = find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(unsupported, nullptr);
  EXPECT_EQ(unsupported->at("subject"), "node:__fork_a__b__c");
  ASSERT_TRUE(unsupported->contains("location"));
  EXPECT_EQ(unsupported->at("location").at("file"), path);
  EXPECT_EQ(unsupported->at("location").at("line"), 8);
}

TEST(RtslPipelineCli, ExitCodeMatrixMatchesTheDiagnosticsCategories) {
  const std::string syntax_path =
      write_temp_file(".rtsl", "system s {\n  cpus 1\n  task a: normal { time [1, 2] estimated\n");
  const CliResult syntax = run_cli({"rtsl", "-f", syntax_path, "--diagnostics", "json"});
  ASSERT_EQ(syntax.exit_code, 2) << syntax.stderr_text;
  const nlohmann::json syntax_envelope = parse_envelope(syntax.stdout_text);
  EXPECT_TRUE(has_diagnostic(syntax_envelope, "E-SYN-102"));

  const std::string validation_path = write_temp_file(
      ".rtsl", "system s {\n  cpus 1\n  task a: periodic { period 0 time [1, 2] estimated }\n}\n");
  const CliResult validation = run_cli({"rtsl", "-f", validation_path, "--diagnostics", "json"});
  ASSERT_EQ(validation.exit_code, 3) << validation.stderr_text;
  const nlohmann::json validation_envelope = parse_envelope(validation.stdout_text);
  EXPECT_NE(find_diagnostic(validation_envelope, "E-VAL-206"), nullptr);

  const std::string unsupported_path = write_temp_file(
      ".rtsl", "system s {\n  cpus 1\n  task sp: sporadic { mit 10 time [1, 2] estimated }\n}\n");
  const CliResult unsupported = run_cli({"rtsl", "-f", unsupported_path, "--diagnostics", "json"});
  ASSERT_EQ(unsupported.exit_code, 4) << unsupported.stderr_text;
  const nlohmann::json unsupported_envelope = parse_envelope(unsupported.stdout_text);
  EXPECT_NE(find_diagnostic(unsupported_envelope, "E-UNS-003"), nullptr);

  const CliResult legacy = run_cli({"rtsl", "-f", validation_path});
  EXPECT_EQ(legacy.exit_code, 1);
  EXPECT_EQ(legacy.stdout_text.find("\"protocol\""), std::string::npos);
  EXPECT_NE(legacy.stderr_text.find("ERROR: " + validation_path), std::string::npos);
  EXPECT_NE(legacy.stderr_text.find("E-VAL-206"), std::string::npos);
}

TEST(RtslPipelineCli, TdgExportRemainsDotOutsideTheRtslSubcommand) {
  const std::string source_path = write_temp_file(".rtsl", kAppendix);
  const std::string tdg_path = test_artifact_name(".tdg.json");
  const std::string dot_path = test_artifact_name(".tdg.dot");

  const CliResult rtsl =
      run_cli({"rtsl", "-f", source_path, "--no-analysis", "--export-tdg", tdg_path});
  ASSERT_EQ(rtsl.exit_code, 0) << rtsl.stderr_text;
  EXPECT_EQ(read_file(tdg_path).front(), '{');

  const CliResult tdg = run_cli({"tdg", "-f", tdg_path, "--no-analysis", "--export-tdg", dot_path});
  ASSERT_EQ(tdg.exit_code, 0) << tdg.stderr_text;
  const std::string dot = read_file(dot_path);
  EXPECT_EQ(dot.find("digraph"), 0U);
  EXPECT_NE(dot.find("__fork_sensor__fusion__log"), std::string::npos);
}

TEST(RtslPipelineCli, ExportedGoldenMatchesTheCommittedTdg) {
#ifndef __GLIBCXX__
  GTEST_SKIP() << "golden files are generated with libstdc++";
#endif
  const std::string exported = test_artifact_name(".tdg.json");
  const CliResult result = run_cli(
      {"rtsl", "-f", "test/golden/rtsl/appendix.rtsl", "--no-analysis", "--export-tdg", exported});
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_EQ(read_file(exported), read_file("test/golden/rtsl/appendix.tdg.json"));
}

TEST(RtslPipelineCli, CommonPipelineOptionsAreAvailableOnRtsl) {
  const std::string path = write_temp_file(".rtsl", kAppendix);
  const std::string metrics_path = test_artifact_name(".metrics.json");
  const CliResult result =
      run_cli({"rtsl", "-f", path, "--max-states", "5000", "--canonicalization", "max-lower",
               "--keep-suspension-clocks", "--reduction", "none", "--horizon", "10000",
               "--export-metrics", metrics_path});
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_FALSE(read_file(metrics_path).empty());
}

TEST(RtslPipelineCli, UnsupportedClassStillWritesRequestedTdgExport) {
  // TDG/WCET exports run before TDG->PTPN lowering, so a supported-class
  // failure (exit 4) does not discard the materialized TDG.
  const std::string path = write_temp_file(".rtsl", R"(system s {
  cpus 2
  cores_per_cpu 1
  policy edf
  task a: normal {
    core 0
    locks mutex1
    time [ [1, 1], [1, 2], [1, 1] ] estimated
  }
  start a
}
)");
  const std::string tdg_path = test_artifact_name(".tdg.json");
  const CliResult result =
      run_cli({"rtsl", "-f", path, "--export-tdg", tdg_path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 4) << result.stderr_text;
  EXPECT_TRUE(std::filesystem::exists(tdg_path));
  EXPECT_EQ(read_file(tdg_path).front(), '{');
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_NE(find_diagnostic(envelope, "E-UNS-001"), nullptr);
}

TEST(RtslPipelineCli, AllocateAcceptsTdgJsonInput) {
  const std::string report_path = test_artifact_name(".allocation.json");
  const CliResult result =
      run_cli({"allocate", "-f", "example/p-bench/initial.json", "-o", report_path,
               "--max-candidates", "1", "--candidate-max-states", "10000", "--diagnostics",
               "json"});
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_TRUE(std::filesystem::exists(report_path));
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("ok"), true);
  EXPECT_EQ(envelope.at("exit_code"), 0);
}
