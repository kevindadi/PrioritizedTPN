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

#include "common/diagnostics.h"

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
  std::ifstream input(path);
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
  return ::testing::TempDir() + "ptpn_diag_" + name + "_" + std::to_string(counter++) + suffix;
}

std::string write_temp_file(const std::string& name, const std::string& contents) {
  const std::string path = test_artifact_name(name);
  std::ofstream output(path);
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

bool has_diagnostic(const nlohmann::json& envelope, const std::string& code) {
  return find_diagnostic(envelope, code) != nullptr;
}

const char* const kBrokenJson = R"({ "nodes": [ })";

const char* const kInvalidTdg = R"({
  "graph": {"name": "invalid"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "task_place_capacity": 1,
    "shared_locks": [],
    "start": ["ghost"],
    "end": ["A"],
    "periodic": [{"task": "A", "period": 0}]
  },
  "nodes": [
    {"id": "A", "type": "task", "core": 5, "time": [[1, 2]]}
  ],
  "edges": [{"source": "A", "target": "missing"}]
})";

const char* const kEdfWithLocks = R"({
  "graph": {"name": "edf_locks"},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "shared_locks": ["mutex1"],
    "policy": "edf",
    "end": ["A", "B"],
    "periodic": [{"task": "A", "period": 10}, {"task": "B", "period": 10}]
  },
  "nodes": [
    {"id": "A", "type": "task", "core": 0, "time": [[1, 1], [1, 1], [1, 1]],
     "locks": ["mutex1"], "deadline": 9},
    {"id": "B", "type": "task", "core": 0, "time": [[1, 1]], "deadline": 9}
  ],
  "edges": []
})";

}  // namespace

TEST(DiagnosticsTest, EnvelopeShape) {
  ptpn::Diagnostics diagnostics;
  ptpn::SourceLocation location;
  location.file = "demo.rtsl";
  location.line = 7;
  location.column = 3;
  location.span = 9;
  diagnostics.add("E-VAL-014", "deadline 120 exceeds period 100", "task:fusion", location,
                  "set deadline <= period, or drop it to inherit the period");

  const nlohmann::json envelope = diagnostics.to_json();
  ASSERT_TRUE(envelope.is_object());
  EXPECT_EQ("ptpn-diagnostics", envelope.at("protocol"));
  EXPECT_EQ(1, envelope.at("version"));
  EXPECT_FALSE(envelope.at("ok").get<bool>());
  EXPECT_EQ(3, envelope.at("exit_code"));
  EXPECT_FALSE(envelope.at("truncated_diagnostics").get<bool>());
  EXPECT_FALSE(envelope.contains("result"));

  ASSERT_EQ(1u, envelope.at("diagnostics").size());
  const nlohmann::json& entry = envelope.at("diagnostics").at(0);
  EXPECT_EQ("E-VAL-014", entry.at("code"));
  EXPECT_EQ("validation", entry.at("category"));
  EXPECT_EQ("error", entry.at("severity"));
  EXPECT_EQ("task:fusion", entry.at("subject"));
  EXPECT_EQ("deadline 120 exceeds period 100", entry.at("message"));
  EXPECT_EQ("set deadline <= period, or drop it to inherit the period", entry.at("fix"));
  ASSERT_TRUE(entry.contains("location"));
  EXPECT_EQ("demo.rtsl", entry.at("location").at("file"));
  EXPECT_EQ(7, entry.at("location").at("line"));
  EXPECT_EQ(3, entry.at("location").at("column"));
  EXPECT_EQ(9, entry.at("location").at("span"));
}

TEST(DiagnosticsTest, EnvelopeOmitsUnknownLocationAndIncludesResult) {
  ptpn::Diagnostics diagnostics;
  diagnostics.add("R-ANA-001", "reachable deadlock", "system:deadlock");
  diagnostics.set_result(nlohmann::json{{"analysis", {{"deadlock", true}}}});

  const nlohmann::json envelope = diagnostics.to_json();
  EXPECT_EQ(6, envelope.at("exit_code"));
  EXPECT_TRUE(envelope.contains("result"));
  EXPECT_TRUE(envelope.at("result").at("analysis").at("deadlock").get<bool>());
  const nlohmann::json& entry = envelope.at("diagnostics").at(0);
  EXPECT_FALSE(entry.contains("location"));
  EXPECT_FALSE(entry.contains("fix"));
}

TEST(DiagnosticsTest, ExitCodePriority) {
  {
    ptpn::Diagnostics diagnostics;
    diagnostics.add("E-PERF-001", "truncated");
    diagnostics.add("R-ANA-001", "deadlock");
    EXPECT_EQ(6, diagnostics.exit_code());
  }
  {
    ptpn::Diagnostics diagnostics;
    diagnostics.add("E-UNS-001", "unsupported");
    diagnostics.add("E-VAL-001", "invalid");
    EXPECT_EQ(3, diagnostics.exit_code());
  }
  {
    ptpn::Diagnostics diagnostics;
    diagnostics.add("E-VAL-001", "invalid");
    diagnostics.add("E-SYN-001", "syntax");
    EXPECT_EQ(2, diagnostics.exit_code());
  }
  {
    ptpn::Diagnostics diagnostics;
    diagnostics.add("E-PERF-001", "truncated");
    EXPECT_EQ(5, diagnostics.exit_code());
  }
  {
    ptpn::Diagnostics diagnostics;
    diagnostics.add("E-SYN-001", "syntax");
    diagnostics.add("E-INT-001", "bug");
    EXPECT_EQ(1, diagnostics.exit_code());
  }
  {
    ptpn::Diagnostics diagnostics;
    diagnostics.add_warning("E-VAL-032", "no task nodes");
    EXPECT_EQ(0, diagnostics.exit_code());
    EXPECT_TRUE(diagnostics.ok());
  }
}

TEST(DiagnosticsTest, CapAndTruncationFlag) {
  ptpn::Diagnostics diagnostics;
  for (int index = 0; index < 12; ++index) {
    diagnostics.add("E-VAL-001", "error " + std::to_string(index));
  }
  EXPECT_EQ(ptpn::Diagnostics::kMaxDiagnostics, diagnostics.size());
  EXPECT_TRUE(diagnostics.truncated());
  EXPECT_EQ(10u, diagnostics.to_json().at("diagnostics").size());
  EXPECT_TRUE(diagnostics.to_json().at("truncated_diagnostics").get<bool>());
}

TEST(DiagnosticsTest, TaintedSubjectsAreSuppressed) {
  ptpn::Diagnostics diagnostics;
  diagnostics.mark_tainted("task:A");
  diagnostics.add("E-VAL-006", "bad core", "task:A");
  diagnostics.add("E-VAL-006", "bad core", "task:B");
  EXPECT_EQ(1u, diagnostics.size());
  EXPECT_EQ("task:B", diagnostics.entries().front().subject);
}

TEST(DiagnosticsCliTest, MalformedJsonEmitsSyntaxDiagnostic) {
  const std::string input = write_temp_file(".json", kBrokenJson);
  const CliResult result = run_cli({"tdg", "-f", input, "--diagnostics", "json"});
  EXPECT_EQ(2, result.exit_code);

  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(2, envelope.value("exit_code", 0));
  EXPECT_FALSE(envelope.value("ok", true));
  const nlohmann::json* diagnostic = find_diagnostic(envelope, "E-SYN-001");
  ASSERT_NE(nullptr, diagnostic);
  EXPECT_EQ("syntax", diagnostic->value("category", ""));
  EXPECT_EQ("error", diagnostic->value("severity", ""));
  ASSERT_TRUE(diagnostic->contains("location"));
  EXPECT_EQ(input, diagnostic->at("location").value("file", ""));
  EXPECT_EQ(1, diagnostic->at("location").value("line", 0));
  EXPECT_GT(diagnostic->at("location").value("column", 0), 0);

  // stdout is reserved for the envelope; human-readable logs move to stderr.
  EXPECT_EQ(std::string::npos, result.stdout_text.find("[JSON]"));
  EXPECT_NE(std::string::npos, result.stderr_text.find("[JSON]"));
}

TEST(DiagnosticsCliTest, ValidationFailuresCarryStableCodesAndSubjects) {
  const std::string input = write_temp_file(".json", kInvalidTdg);
  const CliResult result = run_cli({"tdg", "-f", input, "--diagnostics", "json", "--no-analysis"});
  EXPECT_EQ(3, result.exit_code);

  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(3, envelope.value("exit_code", 0));
  EXPECT_TRUE(has_diagnostic(envelope, "E-VAL-006"));
  EXPECT_TRUE(has_diagnostic(envelope, "E-VAL-019"));
  EXPECT_TRUE(has_diagnostic(envelope, "E-VAL-020"));
  EXPECT_TRUE(has_diagnostic(envelope, "E-VAL-027"));

  const nlohmann::json* unknown_target = find_diagnostic(envelope, "E-VAL-019");
  ASSERT_NE(nullptr, unknown_target);
  EXPECT_EQ("edge:A->missing", unknown_target->value("subject", ""));

  const nlohmann::json* warning = find_diagnostic(envelope, "E-VAL-031");
  ASSERT_NE(nullptr, warning);
  EXPECT_EQ("warning", warning->value("severity", ""));
}

TEST(DiagnosticsCliTest, UnsupportedClassEmitsUnsDiagnostic) {
  const std::string input = write_temp_file(".json", kEdfWithLocks);
  const CliResult result = run_cli({"tdg", "-f", input, "--diagnostics", "json"});
  EXPECT_EQ(4, result.exit_code);

  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(4, envelope.value("exit_code", 0));
  const nlohmann::json* diagnostic = find_diagnostic(envelope, "E-UNS-001");
  ASSERT_NE(nullptr, diagnostic);
  EXPECT_EQ("unsupported", diagnostic->value("category", ""));
  EXPECT_EQ("task:A", diagnostic->value("subject", ""));
}

TEST(DiagnosticsCliTest, WithoutFlagsBehaviorIsUnchanged) {
  const std::string input = write_temp_file(".json", kBrokenJson);

  const CliResult plain = run_cli({"tdg", "-f", input});
  EXPECT_EQ(1, plain.exit_code);
  // Legacy behavior: no envelope on stdout and the parse-failure log there.
  EXPECT_FALSE(nlohmann::json::accept(plain.stdout_text));
  EXPECT_NE(std::string::npos, plain.stdout_text.find("[JSON] Parse error"));

  const CliResult engaged = run_cli({"tdg", "-f", input, "--diagnostics", "json"});
  EXPECT_EQ(2, engaged.exit_code);
  EXPECT_TRUE(nlohmann::json::accept(engaged.stdout_text));
  EXPECT_EQ(std::string::npos, engaged.stdout_text.find("[JSON] Parse error"));
}

TEST(DiagnosticsCliTest, DiagnosticsFileReceivesEnvelope) {
  const std::string input = write_temp_file(".json", kBrokenJson);
  const std::string envelope_path = test_artifact_name(".diagnostics.json");
  const CliResult result = run_cli({"tdg", "-f", input, "--diagnostics-file", envelope_path});
  EXPECT_EQ(2, result.exit_code);
  EXPECT_TRUE(result.stdout_text.empty());

  const nlohmann::json envelope = parse_envelope(read_file(envelope_path));
  EXPECT_EQ(2, envelope.value("exit_code", 0));
  EXPECT_TRUE(has_diagnostic(envelope, "E-SYN-001"));
  std::filesystem::remove(envelope_path);
}

TEST(DiagnosticsCliTest, TruncationWithoutCounterexampleIsPerformance) {
  const CliResult result = run_cli(
      {"tdg", "-f", "example/motivating-examples/hw.json", "--diagnostics", "json", "-m", "10"});
  EXPECT_EQ(5, result.exit_code);

  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(5, envelope.value("exit_code", 0));
  ASSERT_TRUE(envelope.contains("result"));
  const nlohmann::json& analysis = envelope.at("result").at("analysis");
  EXPECT_TRUE(analysis.at("truncated").get<bool>());
  EXPECT_TRUE(analysis.at("counterexample").is_null());

  const nlohmann::json* diagnostic = find_diagnostic(envelope, "E-PERF-001");
  ASSERT_NE(nullptr, diagnostic);
  EXPECT_EQ("performance", diagnostic->value("category", ""));
  EXPECT_EQ("warning", diagnostic->value("severity", ""));
  EXPECT_NE(std::string::npos,
            diagnostic->value("fix", "").find("no counterexample found; raising the limit may help"));
}

TEST(DiagnosticsCliTest, UnreleasedTaskIsHinted) {
  const std::string path = write_temp_file(".rtsl", R"(system s {
  cpus 1
  cores_per_cpu 1
  time_unit ms
  policy fixed
  task a: periodic { period 10ms deadline 10ms core 0 time [1, 1] estimated }
  task orphan: aperiodic { core 0 time [1, 1] estimated }
  start a
}
)");
  const CliResult result = run_cli({"rtsl", "-f", path, "--diagnostics", "json"});
  EXPECT_EQ(0, result.exit_code);

  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  const nlohmann::json* diagnostic = find_diagnostic(envelope, "R-ANA-004");
  ASSERT_NE(nullptr, diagnostic);
  EXPECT_EQ("hint", diagnostic->value("severity", ""));
  EXPECT_EQ("task:orphan", diagnostic->value("subject", ""));
  EXPECT_EQ("schedulable", envelope.at("result").at("analysis").value("schedulability", ""));
}

TEST(DiagnosticsCliTest, ResourceBudgetsReportPartialResults) {
  // A wall-clock budget that is already exhausted stops the exploration with a
  // performance diagnostic and exit 5 instead of running to completion.
  const CliResult timeout = run_cli({"tdg", "-f", "example/s-bench/a.json", "--diagnostics",
                                     "json", "--timeout", "0.000001"});
  EXPECT_EQ(5, timeout.exit_code);
  {
    const nlohmann::json envelope = parse_envelope(timeout.stdout_text);
    const nlohmann::json* diagnostic = find_diagnostic(envelope, "E-PERF-003");
    ASSERT_NE(nullptr, diagnostic);
    EXPECT_EQ("performance", diagnostic->value("category", ""));
    EXPECT_EQ("system:timeout", diagnostic->value("subject", ""));
    EXPECT_FALSE(has_diagnostic(envelope, "E-PERF-001"));
    const nlohmann::json& analysis = envelope.at("result").at("analysis");
    EXPECT_TRUE(analysis.at("truncated").get<bool>());
    EXPECT_EQ("unknown", analysis.value("schedulability", ""));
  }

  // A memory ceiling below the process baseline stops it as well.
  const CliResult memory = run_cli({"tdg", "-f", "example/s-bench/a.json", "--diagnostics", "json",
                                    "--max-memory", "1"});
  EXPECT_EQ(5, memory.exit_code);
  {
    const nlohmann::json envelope = parse_envelope(memory.stdout_text);
    const nlohmann::json* diagnostic = find_diagnostic(envelope, "E-PERF-004");
    ASSERT_NE(nullptr, diagnostic);
    EXPECT_EQ("system:max_memory", diagnostic->value("subject", ""));
    EXPECT_TRUE(envelope.at("result").at("analysis").at("truncated").get<bool>());
  }

  // Generous budgets leave the run untouched.
  const CliResult normal = run_cli({"tdg", "-f", "example/s-bench/a.json", "--diagnostics", "json",
                                    "--timeout", "60", "--max-memory", "4000"});
  EXPECT_EQ(0, normal.exit_code);
  EXPECT_EQ("schedulable",
            parse_envelope(normal.stdout_text).at("result").at("analysis").value("schedulability", ""));
}

TEST(DiagnosticsCliTest, CounterexampleDominatesTruncation) {
  // A proven deadline miss (R-ANA-002) inside the explored prefix still wins
  // over the truncation warning (E-PERF-001): exit 6, not 5. A max-states cut
  // no longer manufactures deadlock counterexamples from dropped successors.
  const CliResult result = run_cli({"rtsl", "-f", "example/diagnostics/deadline-miss.rtsl",
                                    "--diagnostics", "json", "-m", "10"});
  EXPECT_EQ(6, result.exit_code);

  const nlohmann::json envelope = parse_envelope(result.stdout_text);
  EXPECT_EQ(6, envelope.value("exit_code", 0));
  EXPECT_TRUE(has_diagnostic(envelope, "R-ANA-002"));
  EXPECT_TRUE(has_diagnostic(envelope, "E-PERF-001"));
  EXPECT_FALSE(has_diagnostic(envelope, "R-ANA-001"));
  ASSERT_TRUE(envelope.contains("result"));
  EXPECT_EQ("deadline", envelope.at("result").at("analysis").value("counterexample", ""));
  EXPECT_EQ("unschedulable",
            envelope.at("result").at("analysis").value("schedulability", ""));
}
