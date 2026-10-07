#include <memory>

#include "lower/convert/conversion.h"
#include "lower/convert/ptpn_converters.h"
#include "lower/convert/tdg_converters.h"

namespace lowering {

ConversionRegistry make_default_registry() {
  ConversionRegistry registry;
  registry.add(std::make_unique<TdgToPtpnConverter>());
  registry.add(std::make_unique<TdgToRomeoConverter>());
  registry.add(std::make_unique<TdgToPpnConverter>());
  registry.add(std::make_unique<PtpnToPpnConverter>());
  return registry;
}

}  // namespace lowering
