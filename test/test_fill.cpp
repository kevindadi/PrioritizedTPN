#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cli_test_helpers.h"
#include "test_rtsl_helpers.h"

namespace {

using namespace ptpn_test;

constexpr const char* kPendingModel =
    "system demo {\n"
    "  cpus 2\n"
    "  cores_per_cpu 1\n"
    "  time_unit ms\n"
    "  task fusion: aperiodic {\n"
    "    core 0\n"
    "    locks mutex1\n"
    "    time measured by llm { runs 10, timeout 2 }\n"
    "  }\n"
    "  task other: normal { core 1 time [1, 1] estimated }\n"
    "  start other\n"
    "}\n";

std::string envelope_of(const std::string& text) {
  const std::size_t start = text.find('{');
  EXPECT_NE(start, std::string::npos);
  return text.substr(start);
}

}  // namespace

TEST(FillTest, FillsPendingIntervalsAndKeepsProvenance) {
  const std::string path = write_temp_file("fill_", ".rtsl", kPendingModel);
  const std::string values =
      write_temp_file("fill_", ".json", R"({"fusion": [[1, 2], [3, 4], [1, 2]]})");

  const CliResult result = run_cli({"fill", "-f", path, "--values", values, "--in-place"});
  ASSERT_EQ(0, result.exit_code) << result.stderr_text;

  const std::string patched = read_file(path);
  EXPECT_NE(std::string::npos,
            patched.find("time [ [1, 2], [3, 4], [1, 2] ] measured by llm { runs 10, timeout 2 }"));

  // The patched model is complete: `check` reports no pending measurements.
  const CliResult check = run_cli({"check", "-f", path, "--diagnostics", "json"});
  ASSERT_EQ(0, check.exit_code) << check.stderr_text;
  const nlohmann::json envelope = nlohmann::json::parse(envelope_of(check.stdout_text));
  EXPECT_TRUE(envelope.at("result").at("pending_measurements").empty());

  // And it lowers and analyzes.
  const CliResult analysis = run_cli({"rtsl", "-f", path, "--diagnostics", "json"});
  EXPECT_EQ(0, analysis.exit_code) << analysis.stderr_text;
}

TEST(FillTest, SingleTaskFormAcceptsBothBracketStyles) {
  const std::string path = write_temp_file("fill_", ".rtsl", kPendingModel);

  for (const std::string& intervals :
       {std::string("[1,2],[3,4],[1,2]"), std::string("[ [1,2], [3,4], [1,2] ]")}) {
    const CliResult result =
        run_cli({"fill", "-f", path, "--task", "fusion", "--intervals", intervals});
    ASSERT_EQ(0, result.exit_code) << result.stderr_text;
    EXPECT_NE(std::string::npos, result.stdout_text.find("time [ [1, 2], [3, 4], [1, 2] ]"));
  }
}

TEST(FillTest, EnvelopeCarriesThePatchedSourceAndPendingList) {
  const std::string path = write_temp_file("fill_", ".rtsl", kPendingModel);
  const std::string values = write_temp_file("fill_", ".json", R"({"fusion": [[1, 1], [2, 2], [1, 1]]})");

  const CliResult result = run_cli({"fill", "-f", path, "--values", values, "--diagnostics", "json"});
  ASSERT_EQ(0, result.exit_code) << result.stderr_text;
  const nlohmann::json envelope = nlohmann::json::parse(envelope_of(result.stdout_text));
  const nlohmann::json& filled = envelope.at("result").at("filled");
  ASSERT_EQ(1u, filled.size());
  EXPECT_EQ("fusion", filled.at(0).at("task").get<std::string>());
  EXPECT_EQ(8, filled.at(0).at("line").get<int>());
  EXPECT_TRUE(envelope.at("result").at("remaining_pending").empty());
  EXPECT_EQ("stdout", envelope.at("result").at("output").get<std::string>());
  EXPECT_NE(std::string::npos,
            envelope.at("result").at("source").get<std::string>().find(
                "time [ [1, 1], [2, 2], [1, 1] ] measured by llm { runs 10, timeout 2 }"));
}

TEST(FillTest, SecondsTimeUnitWritesMillisecondSuffixes) {
  const std::string path = write_temp_file(
      "fill_", ".rtsl",
      "system seconds {\n"
      "  cpus 1\n"
      "  cores_per_cpu 1\n"
      "  time_unit s\n"
      "  task a: periodic {\n"
      "    period 2s\n"
      "    core 0\n"
      "    time measured by llm { runs 5 }\n"
      "  }\n"
      "  start a\n"
      "}\n");
  const CliResult result =
      run_cli({"fill", "-f", path, "--task", "a", "--intervals", "[1500,2000]"});
  ASSERT_EQ(0, result.exit_code) << result.stderr_text;
  EXPECT_NE(std::string::npos,
            result.stdout_text.find("time [1500ms, 2000ms] measured by llm { runs 5 }"));

  // The value is milliseconds: the analysis reports the same WCRT as a model
  // that declares 1500..2000 ms directly.
  const std::string filled = write_temp_file("fill_", ".rtsl", result.stdout_text);
  const CliResult analysis = run_cli({"rtsl", "-f", filled, "--diagnostics", "json"});
  ASSERT_EQ(0, analysis.exit_code) << analysis.stderr_text;
  const nlohmann::json envelope = nlohmann::json::parse(envelope_of(analysis.stdout_text));
  const nlohmann::json& tasks = envelope.at("result").at("analysis").at("tasks");
  ASSERT_EQ(1u, tasks.size());
  EXPECT_EQ(2000, tasks.at(0).at("wcrt").get<int>());
}

TEST(FillTest, OverwriteReplacesExistingIntervals) {
  const std::string path = write_temp_file("fill_", ".rtsl", kPendingModel);
  const std::string first = write_temp_file("fill_", ".json", R"({"fusion": [[1, 2], [3, 4], [1, 2]]})");
  const std::string second = write_temp_file("fill_", ".json", R"({"fusion": [[5, 6], [7, 8], [5, 6]]})");
  ASSERT_EQ(0, run_cli({"fill", "-f", path, "--values", first, "--in-place"}).exit_code);

  // Without --overwrite the second fill refuses to touch the filled task.
  const CliResult refused =
      run_cli({"fill", "-f", path, "--values", second, "--diagnostics", "json"});
  ASSERT_EQ(3, refused.exit_code);
  EXPECT_NE(std::string::npos, refused.stdout_text.find("E-VAL-226"));

  const CliResult overwritten =
      run_cli({"fill", "-f", path, "--values", second, "--overwrite", "--in-place"});
  ASSERT_EQ(0, overwritten.exit_code) << overwritten.stderr_text;
  EXPECT_NE(std::string::npos,
            read_file(path).find("time [ [5, 6], [7, 8], [5, 6] ] measured by llm"));
}

TEST(FillTest, RejectsValuesThatDoNotMatchTheModel) {
  const std::string path = write_temp_file("fill_", ".rtsl", kPendingModel);

  // Unknown task.
  {
    const CliResult result =
        run_cli({"fill", "-f", path, "--task", "nosuch", "--intervals", "[1,2]",
                 "--diagnostics", "json"});
    ASSERT_EQ(3, result.exit_code);
    EXPECT_NE(std::string::npos, result.stdout_text.find("task:nosuch"));
  }
  // Wrong segment count (fusion has one lock, so three segments are required).
  {
    const CliResult result =
        run_cli({"fill", "-f", path, "--task", "fusion", "--intervals", "[1,2]",
                 "--diagnostics", "json"});
    ASSERT_EQ(3, result.exit_code);
    EXPECT_NE(std::string::npos, result.stdout_text.find("needs 3 interval(s)"));
  }
  // Backwards interval.
  {
    const CliResult result =
        run_cli({"fill", "-f", path, "--task", "fusion", "--intervals", "[3,2],[1,1],[1,1]",
                 "--diagnostics", "json"});
    ASSERT_EQ(3, result.exit_code);
    EXPECT_NE(std::string::npos, result.stdout_text.find("min > max"));
  }
  // Missing input: neither --values nor --task.
  {
    const CliResult result = run_cli({"fill", "-f", path, "--diagnostics", "json"});
    ASSERT_EQ(3, result.exit_code);
    EXPECT_NE(std::string::npos, result.stdout_text.find("no timing supplied"));
  }
}

TEST(FillTest, MalformedValuesFileIsASyntaxError) {
  const std::string path = write_temp_file("fill_", ".rtsl", kPendingModel);
  const std::string values = write_temp_file("fill_", ".json", "{\"fusion\": [[1,2],");

  const CliResult result =
      run_cli({"fill", "-f", path, "--values", values, "--diagnostics", "json"});
  ASSERT_EQ(2, result.exit_code);
  EXPECT_NE(std::string::npos, result.stdout_text.find("E-SYN-001"));
}
