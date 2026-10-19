#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

#include "cli_test_helpers.h"

namespace {

std::string missing_parent_path(const std::string& suffix) {
  const std::string directory = ptpn_test::test_artifact_name("out_dir_", "");
  std::filesystem::remove_all(directory);
  return directory + "/nested/out" + suffix;
}

bool mentions_success_log(const std::string& stderr_text, const std::string& kind) {
  return stderr_text.find(kind + " exported to") != std::string::npos ||
         stderr_text.find("[DOT] Exported") != std::string::npos;
}

}  // namespace

TEST(OutputFailures, TdgDotMissingParentDiagnosticsMode) {
  const std::string path = missing_parent_path(".dot");
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"tdg", "-f", "example/p-bench/initial.json", "--no-analysis",
                          "--export-tdg", path, "--diagnostics", "json"});

  ASSERT_EQ(result.exit_code, 1) << result.stderr_text;
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("exit_code"), 1);
  const nlohmann::json* diagnostic = ptpn_test::find_diagnostic(envelope, "E-INT-002");
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->at("category"), "internal");
  EXPECT_EQ(diagnostic->at("subject"), "output:" + path);
  EXPECT_EQ(ptpn_test::find_diagnostic(envelope, "E-INT-001"), nullptr);
  EXPECT_FALSE(mentions_success_log(result.stderr_text, "TDG DOT"));
}

TEST(OutputFailures, TdgDotMissingParentLegacyMode) {
  const std::string path = missing_parent_path(".dot");
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"tdg", "-f", "example/p-bench/initial.json", "--no-analysis",
                          "--export-tdg", path});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.stdout_text.find("\"protocol\""), std::string::npos);
  EXPECT_FALSE(mentions_success_log(result.stderr_text, "TDG DOT"));
}

TEST(OutputFailures, WcetMissingParentDiagnosticsMode) {
  const std::string path = missing_parent_path(".json");
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"tdg", "-f", "example/p-bench/initial.json", "--no-analysis",
                          "--export-wcet", path, "--diagnostics", "json"});

  ASSERT_EQ(result.exit_code, 1) << result.stderr_text;
  const nlohmann::json envelope = ptpn_test::parse_envelope(result.stdout_text);
  EXPECT_EQ(envelope.at("exit_code"), 1);
  const nlohmann::json* diagnostic = ptpn_test::find_diagnostic(envelope, "E-INT-002");
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->at("subject"), "output:" + path);
  EXPECT_EQ(ptpn_test::find_diagnostic(envelope, "E-INT-001"), nullptr);
  EXPECT_FALSE(mentions_success_log(result.stderr_text, "WCET JSON"));
}

TEST(OutputFailures, WcetMissingParentLegacyMode) {
  const std::string path = missing_parent_path(".json");
  const ptpn_test::CliResult result =
      ptpn_test::run_cli({"tdg", "-f", "example/p-bench/initial.json", "--no-analysis",
                          "--export-wcet", path});

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.stdout_text.find("\"protocol\""), std::string::npos);
  EXPECT_FALSE(mentions_success_log(result.stderr_text, "WCET JSON"));
}

TEST(OutputFailures, SuccessfulExportsKeepTheirContent) {
  const std::string dot_path = ptpn_test::test_artifact_name("out_ok_", ".dot");
  const ptpn_test::CliResult dot =
      ptpn_test::run_cli({"tdg", "-f", "example/p-bench/initial.json", "--no-analysis",
                          "--export-tdg", dot_path});
  ASSERT_EQ(dot.exit_code, 0) << dot.stderr_text;
  const std::string dot_text = ptpn_test::read_file(dot_path);
  EXPECT_EQ(dot_text.find("digraph G"), 0U);
  EXPECT_NE(dot_text.find("A [label"), std::string::npos);
  EXPECT_NE(dot_text.find("A -> B"), std::string::npos);

  const std::string wcet_path = ptpn_test::test_artifact_name("out_ok_", ".json");
  const ptpn_test::CliResult wcet =
      ptpn_test::run_cli({"tdg", "-f", "example/p-bench/initial.json", "--no-analysis",
                          "--export-wcet", wcet_path});
  ASSERT_EQ(wcet.exit_code, 0) << wcet.stderr_text;
  const nlohmann::json wcet_json =
      nlohmann::json::parse(ptpn_test::read_file(wcet_path), nullptr, false);
  ASSERT_FALSE(wcet_json.is_discarded());
  ASSERT_TRUE(wcet_json.contains("tasks"));
  EXPECT_FALSE(wcet_json.at("tasks").empty());
  EXPECT_TRUE(wcet_json.at("tasks").at(0).contains("name"));
  EXPECT_TRUE(wcet_json.at("tasks").at(0).contains("wcet"));
  EXPECT_TRUE(wcet_json.at("all_certified").is_boolean());
}

TEST(OutputFailures, DevFullReportsDelayedWriteFailures) {
#if defined(__linux__)
  if (!std::filesystem::exists("/dev/full")) {
    GTEST_SKIP() << "/dev/full is not available";
  }

  const ptpn_test::CliResult dot =
      ptpn_test::run_cli({"tdg", "-f", "example/p-bench/initial.json", "--no-analysis",
                          "--export-tdg", "/dev/full", "--diagnostics", "json"});
  ASSERT_EQ(dot.exit_code, 1) << dot.stderr_text;
  const nlohmann::json dot_envelope = ptpn_test::parse_envelope(dot.stdout_text);
  const nlohmann::json* dot_diagnostic =
      ptpn_test::find_diagnostic(dot_envelope, "E-INT-002");
  ASSERT_NE(dot_diagnostic, nullptr);
  EXPECT_EQ(dot_diagnostic->at("subject"), "output:/dev/full");
  EXPECT_FALSE(mentions_success_log(dot.stderr_text, "TDG DOT"));

  const ptpn_test::CliResult wcet =
      ptpn_test::run_cli({"tdg", "-f", "example/p-bench/initial.json", "--no-analysis",
                          "--export-wcet", "/dev/full", "--diagnostics", "json"});
  ASSERT_EQ(wcet.exit_code, 1) << wcet.stderr_text;
  const nlohmann::json wcet_envelope = ptpn_test::parse_envelope(wcet.stdout_text);
  const nlohmann::json* wcet_diagnostic =
      ptpn_test::find_diagnostic(wcet_envelope, "E-INT-002");
  ASSERT_NE(wcet_diagnostic, nullptr);
  EXPECT_EQ(wcet_diagnostic->at("subject"), "output:/dev/full");
  EXPECT_FALSE(mentions_success_log(wcet.stderr_text, "WCET JSON"));
#else
  GTEST_SKIP() << "/dev/full is a Linux-only test";
#endif
}
