#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>
#include <variant>

#include "export/export_dot.h"
#include "export/export_ptpn.h"
#include "lower/convert/conversion.h"
#include "lower/convert/ptpn_converters.h"
#include "lower/convert/tdg_converters.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "lower/tdg2ptopner/export_ppn.h"
#include "lower/tdg2romeo/romeo_model.h"
#include "model/petri.h"
#include "tdg_test_helpers.h"

namespace {

using lowering::Capability;
using lowering::ConversionOptions;
using lowering::ConversionRegistry;
using lowering::ConversionResult;
using lowering::Model;
using lowering::ModelKind;

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("cannot read " + path);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string render_ptpn_dot(const petri::PTPN& net) {
  return petri::exporting::render_dot(petri::exporting::build_export_model(net));
}

tdg::TDG load_tdg(const std::string& path) {
  return ptpn_test::load_tdg(path);
}

}  // namespace

TEST(ConversionTest, TdgToPtpnMatchesDirectLowering) {
  const tdg::TDG tdg = load_tdg("example/motivating-examples/hw.json");
  ConversionOptions options;
  options.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESUME;
  const ConversionRegistry registry = lowering::make_default_registry();

  const ConversionResult result = registry.convert(Model{tdg}, ModelKind::PTPN, options);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(lowering::kind_of(result.model), ModelKind::PTPN);
  const auto* converted = std::get_if<petri::PTPN>(&result.model);
  ASSERT_NE(converted, nullptr);

  tdg::TDG configured = tdg;
  configured.policy = options.policy;
  petri::PTPN direct;
  converter::TDG2PN::transform(configured, direct);
  EXPECT_EQ(render_ptpn_dot(*converted), render_ptpn_dot(direct));
}

TEST(ConversionTest, TdgToRomeoMatchesDirectEncoding) {
  const tdg::TDG tdg = load_tdg("example/s-bench/a.json");
  ConversionOptions options;
  options.romeo_format = romeo::RomeoFormat::SchedulingNet;
  const ConversionRegistry registry = lowering::make_default_registry();

  const ConversionResult result = registry.convert(Model{tdg}, ModelKind::ROMEO, options);
  ASSERT_TRUE(result.ok) << result.error;
  const auto* converted = std::get_if<romeo::RomeoModel>(&result.model);
  ASSERT_NE(converted, nullptr);

  romeo::RomeoExportOptions direct_options;
  direct_options.format = options.romeo_format;
  EXPECT_EQ(romeo::render_romeo_cts(*converted),
            romeo::render_romeo_cts(romeo::build_romeo_model(tdg, direct_options)));
}

TEST(ConversionTest, TdgToPpnMatchesGoldenOutput) {
  const tdg::TDG tdg = load_tdg("example/p-bench/no-period.json");
  const ConversionRegistry registry = lowering::make_default_registry();

  const ConversionResult result = registry.convert(Model{tdg}, ModelKind::PPN, ConversionOptions{});
  ASSERT_TRUE(result.ok) << result.error;
  const auto* converted = std::get_if<ptopner_export::PpnModel>(&result.model);
  ASSERT_NE(converted, nullptr);
  EXPECT_EQ(ptopner_export::ppn_to_string(*converted), read_text("test/golden/no-period.ppn"));
}

TEST(ConversionTest, PtpnToPpnMatchesGoldenOutput) {
  tdg::TDG tdg = load_tdg("example/p-bench/no-period.json");
  tdg.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);

  const ConversionRegistry registry = lowering::make_default_registry();
  const ConversionResult result =
      registry.convert(Model{ptpn}, ModelKind::PPN, ConversionOptions{});
  ASSERT_TRUE(result.ok) << result.error;
  const auto* converted = std::get_if<ptopner_export::PpnModel>(&result.model);
  ASSERT_NE(converted, nullptr);
  EXPECT_EQ(ptopner_export::ppn_to_string(*converted), read_text("test/golden/no-period.ppn"));
}

TEST(ConversionTest, RegistryComposesTdgToPpnViaPtpnHub) {
  ConversionRegistry registry;
  registry.add(std::make_unique<lowering::TdgToPtpnConverter>());
  registry.add(std::make_unique<lowering::PtpnToPpnConverter>());
  EXPECT_FALSE(registry.has_direct(ModelKind::TDG, ModelKind::PPN));

  const tdg::TDG tdg = load_tdg("example/p-bench/no-period.json");
  ConversionOptions options;
  options.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;

  const ConversionResult result = registry.convert(Model{tdg}, ModelKind::PPN, options);
  ASSERT_TRUE(result.ok) << result.error;
  const auto* converted = std::get_if<ptopner_export::PpnModel>(&result.model);
  ASSERT_NE(converted, nullptr);
  EXPECT_EQ(ptopner_export::ppn_to_string(*converted), read_text("test/golden/no-period.ppn"));
}

TEST(ConversionTest, PtpnToPpnRejectsLockResourcePlaces) {
  const std::string json = R"({
    "graph": {"name": "Locked"},
    "configuration": {
      "num_cpus": 1,
      "cores_per_cpu": 1,
      "shared_locks": ["mutex1"],
      "policy": "fixed_prior_with_restart"
    },
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0,
       "time": [[1, 1], [2, 2], [1, 1]], "locks": ["mutex1"]}
    ],
    "edges": []
  })";
  tdg::TDG locked;
  locked.parse_json_string(json);

  const ConversionRegistry registry = lowering::make_default_registry();
  ConversionOptions options;
  options.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;

  const ConversionResult to_ptpn = registry.convert(Model{locked}, ModelKind::PTPN, options);
  ASSERT_TRUE(to_ptpn.ok) << to_ptpn.error;

  const ConversionResult to_ppn = registry.convert(to_ptpn.model, ModelKind::PPN, options);
  EXPECT_FALSE(to_ppn.ok);
  EXPECT_NE(to_ppn.error.find("lock"), std::string::npos);
}

TEST(ConversionTest, PtpnToPpnRejectsNonPointIntervals) {
  const tdg::TDG tdg = load_tdg("example/motivating-examples/hw.json");
  const ConversionRegistry registry = lowering::make_default_registry();
  ConversionOptions options;
  options.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESUME;

  const ConversionResult to_ptpn = registry.convert(Model{tdg}, ModelKind::PTPN, options);
  ASSERT_TRUE(to_ptpn.ok) << to_ptpn.error;

  const ConversionResult to_ppn = registry.convert(to_ptpn.model, ModelKind::PPN, options);
  EXPECT_FALSE(to_ppn.ok);
  EXPECT_NE(to_ppn.error.find("non-point"), std::string::npos);
}

TEST(ConversionTest, UnsupportedPathFailsGracefully) {
  tdg::TDG tdg = load_tdg("example/s-bench/a.json");
  tdg.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;
  petri::PTPN ptpn;
  converter::TDG2PN::transform(tdg, ptpn);

  const ConversionRegistry registry = lowering::make_default_registry();
  const ConversionResult result =
      registry.convert(Model{ptpn}, ModelKind::ROMEO, ConversionOptions{});
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("no conversion path"), std::string::npos);
}

TEST(ConversionTest, IdentityConversionReturnsInput) {
  const tdg::TDG tdg = load_tdg("example/s-bench/a.json");
  const ConversionRegistry registry = lowering::make_default_registry();
  const ConversionResult result = registry.convert(Model{tdg}, ModelKind::TDG, ConversionOptions{});
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(lowering::kind_of(result.model), ModelKind::TDG);
}

TEST(ConversionTest, DirectEdgesDeclareCapabilities) {
  EXPECT_EQ(lowering::TdgToPtpnConverter().capability(), Capability::Lossless);
  EXPECT_EQ(lowering::TdgToRomeoConverter().capability(), Capability::Lossy);
  EXPECT_EQ(lowering::TdgToPpnConverter().capability(), Capability::Conditional);
  EXPECT_EQ(lowering::PtpnToPpnConverter().capability(), Capability::Conditional);
}
