#ifndef PTPN_TEST_CLI_TEST_HELPERS_H
#define PTPN_TEST_CLI_TEST_HELPERS_H

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

#ifndef PTPN_CLI_PATH
#  define PTPN_CLI_PATH "ptpn"
#endif

namespace ptpn_test {

inline std::string shell_quote(const std::string& value) {
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

inline std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

// Unique path under the test temp directory. `prefix` keeps artifacts from
// different test files apart even when the test names collide.
inline std::string test_artifact_name(const std::string& prefix, const std::string& suffix) {
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
  return ::testing::TempDir() + prefix + name + "_" + std::to_string(counter++) + suffix;
}

inline std::string write_temp_file(const std::string& prefix, const std::string& suffix,
                                   const std::string& contents) {
  const std::string path = test_artifact_name(prefix, suffix);
  std::ofstream output(path, std::ios::binary);
  output << contents;
  output.close();
  return path;
}

inline int exit_code_from_status(int status) {
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

inline CliResult run_cli(const std::vector<std::string>& arguments) {
  const std::string stdout_path = test_artifact_name("cli_", ".stdout");
  const std::string stderr_path = test_artifact_name("cli_", ".stderr");

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

inline nlohmann::json parse_envelope(const std::string& text) {
  nlohmann::json envelope = nlohmann::json::parse(text, nullptr, false);
  EXPECT_FALSE(envelope.is_discarded());
  EXPECT_TRUE(envelope.is_object());
  return envelope;
}

inline const nlohmann::json* find_diagnostic(const nlohmann::json& envelope,
                                             const std::string& code) {
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

inline bool has_diagnostic(const nlohmann::json& envelope, const std::string& code) {
  return find_diagnostic(envelope, code) != nullptr;
}

inline std::vector<nlohmann::json> diagnostics_with_code(const nlohmann::json& envelope,
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

}  // namespace ptpn_test

#endif  // PTPN_TEST_CLI_TEST_HELPERS_H
