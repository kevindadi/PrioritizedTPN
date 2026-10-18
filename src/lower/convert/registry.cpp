#include <string>

#include "lower/convert/conversion.h"

namespace lowering {

void ConversionRegistry::add(std::unique_ptr<Converter> converter) {
  converters_.push_back(std::move(converter));
}

const Converter* ConversionRegistry::find_direct(ModelKind from, ModelKind to) const {
  for (const auto& converter : converters_) {
    if (converter->from() == from && converter->to() == to) {
      return converter.get();
    }
  }
  return nullptr;
}

bool ConversionRegistry::has_direct(ModelKind from, ModelKind to) const {
  return find_direct(from, to) != nullptr;
}

ConversionResult ConversionRegistry::convert(const Model& input, ModelKind target,
                                             const ConversionOptions& options) const {
  const ModelKind source = kind_of(input);
  if (source == target) {
    return ConversionResult::success(input);
  }

  if (const Converter* direct = find_direct(source, target)) {
    return direct->convert(input, options);
  }

  // No direct edge: route through the canonical hub (PTPN) when possible.
  if (source != ModelKind::PTPN && target != ModelKind::PTPN) {
    const Converter* to_hub = find_direct(source, ModelKind::PTPN);
    const Converter* from_hub = find_direct(ModelKind::PTPN, target);
    if (to_hub != nullptr && from_hub != nullptr) {
      ConversionResult intermediate = to_hub->convert(input, options);
      if (!intermediate.ok) {
        return intermediate;
      }
      return from_hub->convert(intermediate.model, options);
    }
  }

  return ConversionResult::failure("no conversion path from " +
                                   std::string(model_kind_name(source)) + " to " +
                                   std::string(model_kind_name(target)));
}

}  // namespace lowering
