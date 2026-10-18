#include "lower/convert/tdg_converters.h"

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "lower/tdg2pn/tdg2pn.h"
#include "lower/tdg2ptopner/ptpn_to_ppn.h"
#include "lower/tdg2ptopner/validate.h"

namespace lowering {

namespace {

const tdg::TDG* as_tdg(const Model& input) {
  return std::get_if<tdg::TDG>(&input);
}

std::string join_errors(const std::vector<std::string>& errors) {
  std::string joined;
  for (const auto& error : errors) {
    if (!joined.empty()) {
      joined += "; ";
    }
    joined += error;
  }
  return joined;
}

}  // namespace

ModelKind TdgToPtpnConverter::from() const {
  return ModelKind::TDG;
}

ModelKind TdgToPtpnConverter::to() const {
  return ModelKind::PTPN;
}

Capability TdgToPtpnConverter::capability() const {
  return Capability::Lossless;
}

ConversionResult TdgToPtpnConverter::convert(const Model& input,
                                             const ConversionOptions& options) const {
  const tdg::TDG* tdg = as_tdg(input);
  if (tdg == nullptr) {
    return ConversionResult::failure("TDG -> PTPN expects a TDG input");
  }

  tdg::TDG configured = *tdg;
  configured.policy = options.policy;

  petri::PTPN ptpn;
  try {
    converter::TDG2PN::transform(configured, ptpn);
  } catch (const std::exception& e) {
    return ConversionResult::failure(e.what());
  }
  return ConversionResult::success(std::move(ptpn));
}

ModelKind TdgToRomeoConverter::from() const {
  return ModelKind::TDG;
}

ModelKind TdgToRomeoConverter::to() const {
  return ModelKind::ROMEO;
}

Capability TdgToRomeoConverter::capability() const {
  return Capability::Lossy;
}

ConversionResult TdgToRomeoConverter::convert(const Model& input,
                                              const ConversionOptions& options) const {
  const tdg::TDG* tdg = as_tdg(input);
  if (tdg == nullptr) {
    return ConversionResult::failure("TDG -> Romeo expects a TDG input");
  }

  romeo::RomeoExportOptions romeo_options;
  romeo_options.format = options.romeo_format;
  romeo_options.explicit_core_places = options.romeo_explicit_core_places;

  try {
    return ConversionResult::success(romeo::build_romeo_model(*tdg, romeo_options));
  } catch (const std::exception& e) {
    return ConversionResult::failure(e.what());
  }
}

ModelKind TdgToPpnConverter::from() const {
  return ModelKind::TDG;
}

ModelKind TdgToPpnConverter::to() const {
  return ModelKind::PPN;
}

Capability TdgToPpnConverter::capability() const {
  return Capability::Conditional;
}

ConversionResult TdgToPpnConverter::convert(const Model& input,
                                            const ConversionOptions& options) const {
  const tdg::TDG* tdg = as_tdg(input);
  if (tdg == nullptr) {
    return ConversionResult::failure("TDG -> PToPNer expects a TDG input");
  }
  (void)options;  // PToPNer always requires the restart-style lowering.

  tdg::TDG configured = *tdg;
  configured.policy = SchedulePolicy::FIXED_PRIOR_WITH_RESTART;

  const auto validation = ptopner_export::validate_for_ptopner(configured);
  if (!validation.ok) {
    return ConversionResult::failure(join_errors(validation.errors));
  }

  petri::PTPN ptpn;
  try {
    converter::TDG2PN::transform(configured, ptpn);
    return ConversionResult::success(ptopner_export::ptpn_to_ppn_model(ptpn));
  } catch (const std::exception& e) {
    return ConversionResult::failure(e.what());
  }
}

}  // namespace lowering
