#ifndef LOWER_CONVERT_PTPN_CONVERTERS_H
#define LOWER_CONVERT_PTPN_CONVERTERS_H

#include "lower/convert/conversion.h"

namespace lowering {

// PTPN -> PToPNer: maps the canonical net to the flat PPN model. Conditional:
// point intervals only and no lock resource places.
class PtpnToPpnConverter : public Converter {
 public:
  [[nodiscard]] ModelKind from() const override;
  [[nodiscard]] ModelKind to() const override;
  [[nodiscard]] Capability capability() const override;
  [[nodiscard]] ConversionResult convert(const Model& input,
                                         const ConversionOptions& options) const override;
};

}  // namespace lowering

#endif  // LOWER_CONVERT_PTPN_CONVERTERS_H
