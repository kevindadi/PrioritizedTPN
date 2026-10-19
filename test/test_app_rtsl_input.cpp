#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "app/rtsl_input.h"
#include "cli_test_helpers.h"
#include "common/diagnostics.h"
#include "dsl/validate.h"

namespace {

bool has_code(const ptpn::Diagnostics& diagnostics, const std::string& code) {
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

app::RtslDocument parse_source(const std::string& source, const std::string& path,
                               dsl::ValidationOptions::Mode mode,
                               ptpn::Diagnostics& diagnostics) {
  std::optional<std::string> read = app::read_rtsl_source(path);
  EXPECT_TRUE(read.has_value());
  return app::parse_and_validate_rtsl(read.has_value() ? std::move(*read) : std::string(), path,
                                      mode, diagnostics);
}

const char* const kLockedOneSegment = R"(system s {
  cpus 2
  task a: normal {
    core 0
    locks mutex1
    time [1, 2] estimated
  }
  start a
}
)";

const char* const kPending = R"(system s {
  cpus 2
  task a: normal {
    core 0
    locks mutex1
    time measured by llm { runs 3, timeout 1 }
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

TEST(AppRtslInput, CheckModeCollectsPendingWithoutLowering) {
  const std::string path = ptpn_test::write_temp_file("input_", ".rtsl", kPending);
  ptpn::Diagnostics diagnostics;
  app::RtslDocument document =
      parse_source(kPending, path, dsl::ValidationOptions::Mode::Check, diagnostics);

  EXPECT_TRUE(document.valid);
  EXPECT_TRUE(diagnostics.ok());
  EXPECT_EQ(document.program.tasks.size(), 1U);

  const auto pending = dsl::collect_pending_measurements(document.program, document.source_map);
  ASSERT_TRUE(pending.is_array());
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.at(0).at("task"), "a");
  EXPECT_EQ(pending.at(0).at("expected_segments"), 3);
}

TEST(AppRtslInput, RtslModeRejectsPendingMeasurements) {
  const std::string path = ptpn_test::write_temp_file("input_", ".rtsl", kPending);
  ptpn::Diagnostics diagnostics;
  app::RtslDocument document =
      parse_source(kPending, path, dsl::ValidationOptions::Mode::Rtsl, diagnostics);

  EXPECT_FALSE(document.valid);
  EXPECT_TRUE(has_code(diagnostics, "E-VAL-213"));
}

TEST(AppRtslInput, CheckModeStopsBeforeLoweringOnlyRules) {
  const std::string path = ptpn_test::write_temp_file("input_", ".rtsl", kLockedOneSegment);
  ptpn::Diagnostics diagnostics;
  app::RtslDocument document =
      parse_source(kLockedOneSegment, path, dsl::ValidationOptions::Mode::Check, diagnostics);

  ASSERT_TRUE(document.valid);
  EXPECT_TRUE(diagnostics.ok());

  ptpn::Diagnostics lowering_diagnostics;
  const std::optional<tdg::TDG> lowered = app::lower_rtsl_document(
      document, dsl::ValidationOptions::Mode::Rtsl, lowering_diagnostics);
  EXPECT_FALSE(lowered.has_value());
  EXPECT_TRUE(has_code(lowering_diagnostics, "E-VAL-015"));
}

TEST(AppRtslInput, AllocateModeExpandsCoreAny) {
  const std::string path = ptpn_test::write_temp_file("input_", ".rtsl", kCoreAny);
  ptpn::Diagnostics validation_diagnostics;
  app::RtslDocument document = parse_source(
      kCoreAny, path, dsl::ValidationOptions::Mode::Allocate, validation_diagnostics);

  ASSERT_TRUE(document.valid);
  EXPECT_TRUE(validation_diagnostics.ok());

  ptpn::Diagnostics lowering_diagnostics;
  const std::optional<tdg::TDG> lowered = app::lower_rtsl_document(
      document, dsl::ValidationOptions::Mode::Allocate, lowering_diagnostics);
  ASSERT_TRUE(lowered.has_value());
  const auto allowed = lowered->task_allowed_cores.find("a");
  ASSERT_NE(allowed, lowered->task_allowed_cores.end());
  EXPECT_EQ(allowed->second, (std::vector<int>{0, 1}));
}
