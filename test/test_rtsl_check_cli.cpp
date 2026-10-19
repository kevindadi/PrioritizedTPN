#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
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

std::string shell_quote(const std::string& value) {
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
}

std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string test_artifact_name(const std::string& suffix) {
  std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
  for (char& character : name) {
    const bool safe = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '_';
    if (!safe) {
      character = '_';
    }
  }
  static int counter = 0;
  return ::testing::TempDir() + "ptpn_rtsl_" + name + "_" + std::to_string(counter++) + suffix;
}

std::string write_temp_file(const std::string& suffix, const std::string& contents) {
  const std::string path = test_artifact_name(suffix);
  std::ofstream output(path, std::ios::binary);
  output << contents;
  output.close();
  return path;
}

int exit_code_from_status(int status) {
#if defined(WIFEXITED) && defined(WEXITSTATUS)
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
  return status;
#endif
}

struct CliResult {
  int exit_code = -1;
  std::string stdout_text;
  std::string stderr_text;
};

CliResult run_cli(const std::vector<std::string>& arguments) {
  const std::string stdout_path = test_artifact_name(".stdout");
  const std::string stderr_path = test_artifact_name(".stderr");

  std::string command = shell_quote(PTPN_CLI_PATH);
  for (const std::string& argument : arguments) {
    command += " " + shell_quote(argument);
  }
  command += " >" + shell_quote(stdout_path) + " 2>" + shell_quote(stderr_path);

  const int status = std::system(command.c_str());
  CliResult result;
  result.exit_code = exit_code_from_status(status);
  result.stdout_text = read_file(stdout_path);
  result.stderr_text = read_file(stderr_path);
  std::filesystem::remove(stdout_path);
  std::filesystem::remove(stderr_path);
  return result;
}

nlohmann::json parse_envelope(const std::string& text) {
  nlohmann::json envelope = nlohmann::json::parse(text, nullptr, false);
  EXPECT_FALSE(envelope.is_discarded());
  EXPECT_TRUE(envelope.is_object());
  return envelope;
}

const nlohmann::json* find_diagnostic(const nlohmann::json& envelope, const std::string& code) {
  if (!envelope.contains("diagnostics") || !envelope.at("diagnostics").is_array()) {
    return nullptr;
  }
  for (const auto& diagnostic : envelope.at("diagnostics")) {
    if (diagnostic.value("code", "") == code) {
      return &diagnostic;
    }
  }
  return nullptr;
}

std::vector<nlohmann::json> diagnostics_with_code(const nlohmann::json& envelope,
                                                  const std::string& code) {
  std::vector<nlohmann::json> matches;
  if (!envelope.contains("diagnostics") || !envelope.at("diagnostics").is_array()) {
    return matches;
  }
  for (const auto& diagnostic : envelope.at("diagnostics")) {
    if (diagnostic.value("code", "") == code) {
      matches.push_back(diagnostic);
    }
  }
  return matches;
}

const char* const kPendingRtsl = R"(system demo {
  cpus 4
  task fusion: aperiodic {
    core {1, 2, 3}
    locks mutex1, mutex2
    time measured by llm { runs 7, timeout 9 }
  }
  task solo: normal {
    time measured by llm { artifact "trace.bin" }
  }
  start fusion
}
)";

TEST(RtslCheckCli, ReportsPendingMeasurementsWithInjectionSpans) {
  const std::string path = write_temp_file(".rtsl", kPendingRtsl);
  const CliResult result = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;

  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("protocol"), "ptpn-diagnostics");
  EXPECT_EQ(envelope.at("version"), 1);
  EXPECT_EQ(envelope.at("ok"), true);
  EXPECT_EQ(envelope.at("exit_code"), 0);
  ASSERT_TRUE(envelope.contains("result"));
  ASSERT_TRUE(envelope.at("result").contains("pending_measurements"));
  const nlohmann::json& pending = envelope.at("result").at("pending_measurements");
  ASSERT_TRUE(pending.is_array());
  ASSERT_EQ(pending.size(), 2U);

  const nlohmann::json& fusion = pending.at(0);
  EXPECT_EQ(fusion.at("task"), "fusion");
  EXPECT_EQ(fusion.at("expected_segments"), 5);
  EXPECT_EQ(fusion.at("runs"), 7);
  EXPECT_EQ(fusion.at("timeout"), 9);
  const nlohmann::json& inject = fusion.at("inject");
  EXPECT_EQ(inject.at("file"), path);
  EXPECT_EQ(inject.at("line"), 6);
  EXPECT_EQ(inject.at("column"), 5);

  const std::string contents = read_file(path);
  const std::size_t start = inject.at("byte_start").get<std::size_t>();
  const std::size_t end = inject.at("byte_end").get<std::size_t>();
  ASSERT_LE(end, contents.size());
  EXPECT_EQ(contents.substr(start, end - start), "time measured by llm { runs 7, timeout 9 }");

  const nlohmann::json& solo = pending.at(1);
  EXPECT_EQ(solo.at("task"), "solo");
  EXPECT_EQ(solo.at("expected_segments"), 1);
  EXPECT_EQ(solo.at("runs"), 20);
  EXPECT_EQ(solo.at("timeout"), 5);
  const nlohmann::json& solo_inject = solo.at("inject");
  const std::string solo_text =
      contents.substr(solo_inject.at("byte_start").get<std::size_t>(),
                      solo_inject.at("byte_end").get<std::size_t>() -
                          solo_inject.at("byte_start").get<std::size_t>());
  EXPECT_EQ(solo_text, "time measured by llm { artifact \"trace.bin\" }");
}

TEST(RtslCheckCli, ValidModelWithoutDiagnosticsOptionStaysLegacy) {
  const std::string path = write_temp_file(
      ".rtsl", "system s {\n  cpus 1\n  task a: normal { time [1, 2] estimated }\n}\n");
  const CliResult result = run_cli({"check", "-f", path});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.stdout_text.find("\"protocol\""), std::string::npos);
  EXPECT_NE(result.stderr_text.find("RTSL check: 1 tasks, 0 edges, ok"), std::string::npos);
}

TEST(RtslCheckCli, SyntaxErrorsUseExitCodeTwo) {
  const std::string path =
      write_temp_file(".rtsl", "system s {\n  cpus 1\n  task a: normal { time [1, 2] estimated\n");
  const CliResult result = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 2) << result.stderr_text;
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("ok"), false);
  EXPECT_EQ(envelope.at("exit_code"), 2);
  EXPECT_FALSE(diagnostics_with_code(envelope, "E-SYN-102").empty());
  EXPECT_FALSE(envelope.contains("result"));
}

TEST(RtslCheckCli, ValidationErrorsUseExitCodeThree) {
  const std::string path = write_temp_file(
      ".rtsl", "system s {\n  cpus 1\n  task a: periodic { period 0 time [1, 2] estimated }\n}\n");
  const CliResult result = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 3) << result.stderr_text;
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("ok"), false);
  const nlohmann::json* period = find_diagnostic(envelope, "E-VAL-206");
  ASSERT_NE(period, nullptr);
  ASSERT_TRUE(period->contains("location"));
  EXPECT_EQ(period->at("location").at("line"), 3);
}

TEST(RtslCheckCli, UnsupportedConstructsUseExitCodeFour) {
  const std::string path = write_temp_file(
      ".rtsl", "system s {\n  cpus 1\n  task sp: sporadic { mit 10 time [1, 2] estimated }\n}\n");
  const CliResult result = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 4) << result.stderr_text;
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("exit_code"), 4);
  const nlohmann::json* sporadic = find_diagnostic(envelope, "E-UNS-003");
  ASSERT_NE(sporadic, nullptr);
  EXPECT_EQ(sporadic->at("category"), "unsupported");
}

TEST(RtslCheckCli, MissingInputFileIsASyntaxError) {
  const std::string path = ::testing::TempDir() + "ptpn_rtsl_definitely_missing.rtsl";
  std::filesystem::remove(path);
  const CliResult result = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 2) << result.stderr_text;
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  const nlohmann::json* open_error = find_diagnostic(envelope, "E-SYN-002");
  ASSERT_NE(open_error, nullptr);
  EXPECT_EQ(open_error->at("location").at("file"), path);
}

TEST(RtslCheckCli, DiagnosticsFileOptionWritesTheEnvelope) {
  const std::string path = write_temp_file(
      ".rtsl", "system s {\n  cpus 1\n  task a: normal { time [1, 2] estimated }\n}\n");
  const std::string diagnostics_path = test_artifact_name(".json");
  const CliResult result = run_cli({"check", "-f", path, "--diagnostics-file", diagnostics_path});
  EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_EQ(result.stdout_text.find("\"protocol\""), std::string::npos);

  const nlohmann::json envelope = parse_envelope(read_file(diagnostics_path));
  EXPECT_EQ(envelope.at("protocol"), "ptpn-diagnostics");
  EXPECT_EQ(envelope.at("ok"), true);
  EXPECT_TRUE(envelope.at("result").at("pending_measurements").is_array());
  std::filesystem::remove(diagnostics_path);
}

TEST(RtslCheckCli, PanicModeReportsEveryErrorInTheEnvelope) {
  const std::string path = write_temp_file(".rtsl", R"(system s {
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
}
)");
  const CliResult result = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 2) << result.stderr_text;
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_GE(diagnostics_with_code(envelope, "E-SYN-102").size(), 5U);
}

TEST(RtslCheckCli, HintsKeepTheCheckSuccessful) {
  const std::string path = write_temp_file(".rtsl", R"(system s {
  cpus 2
  task i: interrupt { core 0 time [1, 2] estimated }
  task l: normal { core any time [1, 2] estimated }
}
)");
  const CliResult result = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("ok"), true);
  EXPECT_EQ(envelope.at("exit_code"), 0);
  const nlohmann::json* allocate = find_diagnostic(envelope, "E-VAL-218");
  ASSERT_NE(allocate, nullptr);
  EXPECT_EQ(allocate->at("severity"), "hint");
  const nlohmann::json* interrupt = find_diagnostic(envelope, "E-UNS-005");
  ASSERT_NE(interrupt, nullptr);
  EXPECT_EQ(interrupt->at("severity"), "hint");
}

TEST(RtslCheckCli, LegacyFailuresExitWithOneAndPrintToStderr) {
  const std::string path = write_temp_file(
      ".rtsl", "system s {\n  cpus 1\n  task a: periodic { period 0 time [1, 2] estimated }\n}\n");
  const CliResult result = run_cli({"check", "-f", path});
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.stdout_text.find("\"protocol\""), std::string::npos);
  EXPECT_NE(result.stderr_text.find("E-VAL-206"), std::string::npos);
  EXPECT_NE(result.stderr_text.find("RTSL check:"), std::string::npos);
}

}  // namespace
