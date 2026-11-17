#include <cstddef>
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

}  // namespace

// The catalog doc now lives in the RT-PN workbench (docs/ptpn/diagnostics.md),
// so this repository only checks the generated catalog itself. Refresh it with
// `tools/gen_diagnostic_codes.py --doc PATH`; the doc-vs-catalog drift check
// belongs to the workbench tooling.
TEST(ExplainTest, CatalogEntriesAreComplete) {
  std::set<std::string> catalog;
  for (const ptpn::DiagnosticCodeInfo& info : ptpn::diagnostic_code_catalog()) {
    EXPECT_TRUE(catalog.insert(info.code).second) << "duplicate catalog entry " << info.code;
    EXPECT_FALSE(info.category.empty()) << info.code;
    EXPECT_FALSE(info.severity.empty()) << info.code;
    EXPECT_FALSE(info.trigger.empty()) << info.code;
  }
  EXPECT_FALSE(catalog.empty());
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
