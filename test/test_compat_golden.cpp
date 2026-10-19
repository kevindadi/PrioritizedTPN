#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include <string>

#include "lower/tdg2pn/tdg2pn.h"
#include "lower/tdg2ptopner/tdg2ptopner.h"
#include "lower/tdg2ptopner/validate.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "tdg_test_helpers.h"

namespace {

using ptpn_test::load_tdg;

// Golden output is generated with libstdc++ (the stdlib used by CI):
//   ./build/ptpn export ptopner -f example/p-bench/no-period.json \
//     -o test/golden/no-period.ppn

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("cannot read " + path);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void expect_matches_golden(const std::string& actual_path, const std::string& golden_path) {
#ifndef __GLIBCXX__
  GTEST_SKIP() << "golden files are generated with libstdc++";
#endif
  EXPECT_EQ(read_text(actual_path), read_text(golden_path))
      << "regenerate " << golden_path << " if the change is intentional";
}

}  // namespace

TEST(PipelineGolden, PtopnerPpnMatchesGolden) {
  tdg::TDG tdg = load_tdg("example/p-bench/no-period.json");
  tdg.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;

  const auto validation = ptopner_export::validate_for_ptopner(tdg);
  ASSERT_TRUE(validation.ok);

  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);

  const std::string out = testing::TempDir() + "ptpn_test_ppn.ppn";
  const auto result = ptopner_export::export_ptpn_to_ppn_file(ptpn, out);
  ASSERT_TRUE(result.success) << result.error_message;
  expect_matches_golden(out, "test/golden/no-period.ppn");
}
