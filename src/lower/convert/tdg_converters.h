#ifndef LOWER_CONVERT_TDG_CONVERTERS_H
#define LOWER_CONVERT_TDG_CONVERTERS_H

#include "lower/convert/conversion.h"

namespace lowering {

// TDG -> PTPN: the pass-based lowering (vertices, edges, bindings, preemption,
// resources). The policy option selects the preemption encoding (resume vs
// restart).
class TdgToPtpnConverter : public Converter {
 public:
  [[nodiscard]] ModelKind from() const override;
  [[nodiscard]] ModelKind to() const override;
  [[nodiscard]] Capability capability() const override;
  [[nodiscard]] ConversionResult convert(const Model& input,
                                         const ConversionOptions& options) const override;
};

// TDG -> Romeo: the direct encoder. It deliberately does not route through
// PTPN, so the scheduling-net / inhibitor-arc output stays byte-for-byte
// identical to the existing tdg2romeo pipeline.
class TdgToRomeoConverter : public Converter {
 public:
  [[nodiscard]] ModelKind from() const override;
  [[nodiscard]] ModelKind to() const override;
  [[nodiscard]] Capability capability() const override;
  [[nodiscard]] ConversionResult convert(const Model& input,
                                         const ConversionOptions& options) const override;
};

// TDG -> PToPNer: validate the TDG (point intervals, no locks, restart policy),
// lower with restart preemption, then map to the flat PPN model.
class TdgToPpnConverter : public Converter {
 public:
  [[nodiscard]] ModelKind from() const override;
  [[nodiscard]] ModelKind to() const override;
  [[nodiscard]] Capability capability() const override;
  [[nodiscard]] ConversionResult convert(const Model& input,
                                         const ConversionOptions& options) const override;
};

}  // namespace lowering

#endif  // LOWER_CONVERT_TDG_CONVERTERS_H
