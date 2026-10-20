#include <cstddef>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cli_test_helpers.h"
#include "common/diagnostic_codes.h"

namespace {

using namespace ptpn_test;

// Extracts the code cells of the documentation tables: a row whose first cell
// is a backticked code, e.g. "| `E-VAL-226` | error | ...".
std::set<std::string> documented_codes(const std::string& path) {
  std::ifstream input(path);
  EXPECT_TRUE(input) << "cannot open " << path;
  std::set<std::string> codes;
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("| `", 0) != 0) {
      continue;
    }
    const std::size_t end = line.find('`', 3);
    if (end == std::string::npos) {
      continue;
    }
    const std::string code = line.substr(3, end - 3);
    const bool looks_like_code = code.size() > 4 && code[1] == '-' &&
                                 code.find('-') != std::string::npos &&
                                 code.find_first_of("0123456789") != std::string::npos;
    if (looks_like_code) {
      codes.insert(code);
    }
  }
  return codes;
}

}  // namespace

TEST(ExplainTest, CatalogMatchesDocumentation) {
  std::set<std::string> catalog;
  for (const ptpn::DiagnosticCodeInfo& info : ptpn::diagnostic_code_catalog()) {
    EXPECT_TRUE(catalog.insert(info.code).second) << "duplicate catalog entry " << info.code;
    EXPECT_FALSE(info.category.empty()) << info.code;
    EXPECT_FALSE(info.severity.empty()) << info.code;
    EXPECT_FALSE(info.trigger.empty()) << info.code;
  }

  const std::set<std::string> documented = documented_codes("docs/diagnostics.md");
  ASSERT_FALSE(documented.empty());

  // Regenerate with tools/gen_diagnostic_codes.py when this fails.
  for (const std::string& code : documented) {
    EXPECT_TRUE(catalog.count(code) != 0)
        << code << " is documented but missing from the catalog (run "
                  "tools/gen_diagnostic_codes.py)";
  }
  for (const std::string& code : catalog) {
    EXPECT_TRUE(documented.count(code) != 0)
        << code << " is in the catalog but not documented in docs/diagnostics.md";
  }
}

TEST(ExplainTest, CommandPrintsDetailsAndListsTheCatalog) {
  const CliResult detail = run_cli({"explain", "E-VAL-226"});
  ASSERT_EQ(0, detail.exit_code) << detail.stderr_text;
  EXPECT_NE(std::string::npos, detail.stdout_text.find("E-VAL-226"));
  EXPECT_NE(std::string::npos, detail.stdout_text.find("[validation/error]"));
  EXPECT_NE(std::string::npos, detail.stdout_text.find("fix:"));

  const CliResult listing = run_cli({"explain"});
  ASSERT_EQ(0, listing.exit_code) << listing.stderr_text;
  for (const ptpn::DiagnosticCodeInfo& info : ptpn::diagnostic_code_catalog()) {
    EXPECT_NE(std::string::npos, listing.stdout_text.find(info.code))
        << info.code << " missing from the listing";
  }
}

TEST(ExplainTest, EnvelopeCarriesTheCatalog) {
  const CliResult listing = run_cli({"explain", "--diagnostics", "json"});
  ASSERT_EQ(0, listing.exit_code) << listing.stderr_text;
  const std::string text = listing.stdout_text;
  const std::size_t start = text.find('{');
  ASSERT_NE(std::string::npos, start);
  const nlohmann::json envelope = nlohmann::json::parse(text.substr(start));
  EXPECT_EQ(ptpn::diagnostic_code_catalog().size(),
            envelope.at("result").at("codes").size());

  const CliResult detail = run_cli({"explain", "R-ANA-002", "--diagnostics", "json"});
  ASSERT_EQ(0, detail.exit_code) << detail.stderr_text;
  const std::size_t detail_start = detail.stdout_text.find('{');
  ASSERT_NE(std::string::npos, detail_start);
  const nlohmann::json detail_envelope =
      nlohmann::json::parse(detail.stdout_text.substr(detail_start));
  const nlohmann::json& entry = detail_envelope.at("result").at("codes").at(0);
  EXPECT_EQ("R-ANA-002", entry.at("code").get<std::string>());
  EXPECT_EQ("analysis", entry.at("category").get<std::string>());
  EXPECT_EQ("`task:<name>` / none", entry.at("subject").get<std::string>());
}

TEST(ExplainTest, UnknownCodeIsASyntaxError) {
  const CliResult result = run_cli({"explain", "E-XXX-001", "--diagnostics", "json"});
  ASSERT_EQ(2, result.exit_code);
  EXPECT_NE(std::string::npos, result.stdout_text.find("E-SYN-003"));
  EXPECT_NE(std::string::npos, result.stdout_text.find("argument:E-XXX-001"));

  // Without the envelope the same failure is plain text with exit code 2.
  const CliResult legacy = run_cli({"explain", "E-XXX-001"});
  EXPECT_EQ(2, legacy.exit_code);
  EXPECT_NE(std::string::npos, legacy.stderr_text.find("unknown diagnostic code"));
}
