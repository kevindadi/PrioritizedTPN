#include "lower/convert/ptpn_converters.h"

#include <exception>

#include "lower/tdg2ptopner/ptpn_to_ppn.h"

namespace lowering {

ModelKind PtpnToPpnConverter::from() const {
  return ModelKind::PTPN;
}

ModelKind PtpnToPpnConverter::to() const {
  return ModelKind::PPN;
}

Capability PtpnToPpnConverter::capability() const {
  return Capability::Conditional;
}

ConversionResult PtpnToPpnConverter::convert(const Model& input,
                                             const ConversionOptions& options) const {
  const petri::PTPN* ptpn = std::get_if<petri::PTPN>(&input);
  if (ptpn == nullptr) {
    return ConversionResult::failure("PTPN -> PToPNer expects a PTPN input");
  }
  (void)options;

  if (!ptpn->lock_places().empty()) {
    return ConversionResult::failure("PToPNer export does not support lock resource places");
  }

  try {
    return ConversionResult::success(ptopner_export::ptpn_to_ppn_model(*ptpn));
  } catch (const std::exception& e) {
    return ConversionResult::failure(e.what());
  }
}

}  // namespace lowering
