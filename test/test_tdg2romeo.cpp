#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include "json/json.h"
#include "tdg/tdg.h"
#include "tdg2romeo/tdg2romeo.h"
#include "tdg_test_helpers.h"

namespace {

using ptpn_test::load_tdg;

std::string render(const tdg::TDG& tdg, romeo::RomeoFormat format, bool explicit_core_places) {
  romeo::RomeoExportOptions opts;
  opts.format = format;
  opts.explicit_core_places = explicit_core_places;
  return romeo::render_romeo_cts(romeo::build_romeo_model(tdg, opts));
}

}  // namespace

TEST(Tdg2RomeoTest, SchedulingNetIncludesCorePlacesAndTaskIntervals) {
  const tdg::TDG tdg = load_tdg("example/p-bench/same-core.json");
  const std::string cts = render(tdg, romeo::RomeoFormat::SchedulingNet, true);

  EXPECT_NE(cts.find("core0=1"), std::string::npos);
  EXPECT_NE(cts.find("Aexec [8,8]"), std::string::npos);
  EXPECT_NE(cts.find("Fexec [3,3]"), std::string::npos);
  EXPECT_NE(cts.find("when (Aentry >= 1 and core0 >= 1)"), std::string::npos);
}

TEST(Tdg2RomeoTest, MultiCoreUsesSeparateCoreGuards) {
  const tdg::TDG tdg = load_tdg("example/p-bench/initial.json");
  const std::string cts = render(tdg, romeo::RomeoFormat::SchedulingNet, true);

  EXPECT_NE(cts.find("core0=1"), std::string::npos);
  EXPECT_NE(cts.find("core1=1"), std::string::npos);
  EXPECT_NE(cts.find("when (Aentry >= 1 and core0 >= 1)"), std::string::npos);
  EXPECT_NE(cts.find("when (Centry >= 1 and core1 >= 1)"), std::string::npos);
}

TEST(Tdg2RomeoTest, InhibitorArcUsesSchedPriorityAndAllowWithoutExecPriority) {
  const tdg::TDG tdg = load_tdg("example/p-bench/same-core.json");
  const std::string cts = render(tdg, romeo::RomeoFormat::InhibitorArc, true);

  EXPECT_NE(cts.find("Asched [0,0]"), std::string::npos);
  EXPECT_NE(cts.find("allow="), std::string::npos);
  EXPECT_NE(cts.find("Aactive"), std::string::npos);

  const auto exec_pos = cts.find(" Aexec [8,8]");
  ASSERT_NE(exec_pos, std::string::npos);
  const auto line_start = cts.rfind(" transition", exec_pos);
  ASSERT_NE(line_start, std::string::npos);
  const auto line_end = cts.find('\n', exec_pos);
  const std::string exec_block = cts.substr(line_start, line_end - line_start);
  EXPECT_EQ(exec_block.find("priority="), std::string::npos);
}

TEST(Tdg2RomeoTest, EdgeIntervalFromLabel) {
  const tdg::TDG tdg = load_tdg("example/p-bench/no-period.json");
  const std::string cts = render(tdg, romeo::RomeoFormat::SchedulingNet, true);
  EXPECT_NE(cts.find("A_to_B [0,0]"), std::string::npos);
}

TEST(Tdg2RomeoTest, ExportToFileSucceeds) {
  const tdg::TDG tdg = load_tdg("example/s-bench/a.json");
  romeo::RomeoExportOptions opts;
  opts.format = romeo::RomeoFormat::SchedulingNet;
  const auto result = romeo::export_tdg_to_romeo_cts(tdg, "/tmp/ptpn_test_a.cts", opts);
  EXPECT_TRUE(result.success) << result.error_message;
}

TEST(Tdg2RomeoTest, SBenchSchedulingNetMatchesGoldenStructure) {
  const tdg::TDG tdg = load_tdg("example/s-bench/a.json");
  const std::string cts = render(tdg, romeo::RomeoFormat::SchedulingNet, true);

  EXPECT_NE(cts.find("Aexec [3,5]"), std::string::npos);
  EXPECT_NE(cts.find("Dexec [8,8]"), std::string::npos);
  EXPECT_NE(cts.find("A_fire [100,100]"), std::string::npos);
  EXPECT_NE(cts.find("D_fire [50,50]"), std::string::npos);
  EXPECT_NE(cts.find("C_consume [0,0]"), std::string::npos);
}
