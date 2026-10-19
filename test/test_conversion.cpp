#include <gtest/gtest.h>
#include <string>
#include <variant>

#include "export/export_dot.h"
#include "export/export_ptpn.h"
#include "lower/convert/conversion.h"
#include "lower/convert/tdg_converters.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "tdg_test_helpers.h"

namespace {

using lowering::Capability;
using lowering::ConversionOptions;
using lowering::ConversionRegistry;
using lowering::ConversionResult;
using lowering::Model;
using lowering::ModelKind;

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

TEST(ConversionTest, MainlineEdgeDeclaresLosslessCapability) {
  EXPECT_EQ(lowering::TdgToPtpnConverter().capability(), Capability::Lossless);
}
