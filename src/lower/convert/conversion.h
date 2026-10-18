#ifndef LOWER_CONVERT_CONVERSION_H
#define LOWER_CONVERT_CONVERSION_H

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "lower/convert/model_kind.h"
#include "lower/tdg2ptopner/ppn_model.h"
#include "lower/tdg2romeo/romeo_model.h"
#include "lower/tdg2romeo/tdg2romeo.h"
#include "model/petri.h"
#include "model/tdg.h"
#include "model/types.h"

namespace lowering {

// The tagged union of every model the framework can carry.
using Model = std::variant<tdg::TDG, petri::PTPN, romeo::RomeoModel, ptopner_export::PpnModel>;

inline ModelKind kind_of(const Model& model) {
  return std::visit(
      [](const auto& value) -> ModelKind {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, tdg::TDG>) {
          return ModelKind::TDG;
        } else if constexpr (std::is_same_v<T, petri::PTPN>) {
          return ModelKind::PTPN;
        } else if constexpr (std::is_same_v<T, romeo::RomeoModel>) {
          return ModelKind::ROMEO;
        } else {
          return ModelKind::PPN;
        }
      },
      model);
}

struct ConversionOptions {
  // Scheduling policy used when lowering a TDG to a PTPN (resume/restart).
  SchedulePolicy policy = SchedulePolicy::FIXED;
  // Romeo encoding knobs (ignored by non-Romeo targets).
  romeo::RomeoFormat romeo_format = romeo::RomeoFormat::SchedulingNet;
  bool romeo_explicit_core_places = true;
};

struct ConversionResult {
  bool ok = false;
  std::string error;
  Model model;

  static ConversionResult success(Model converted) {
    ConversionResult result;
    result.ok = true;
    result.model = std::move(converted);
    return result;
  }

  static ConversionResult failure(std::string message) {
    ConversionResult result;
    result.error = std::move(message);
    return result;
  }
};

// Semantic guarantee documented by one converter edge.
enum class Capability {
  Lossless,     // structure and semantics are preserved
  Lossy,        // the target is a strict encoding of the source
  Conditional,  // valid only when the source satisfies checked preconditions
};

class Converter {
 public:
  virtual ~Converter() = default;

  [[nodiscard]] virtual ModelKind from() const = 0;
  [[nodiscard]] virtual ModelKind to() const = 0;
  [[nodiscard]] virtual Capability capability() const = 0;
  [[nodiscard]] virtual ConversionResult convert(const Model& input,
                                                 const ConversionOptions& options) const = 0;
};

// Holds the supported conversion edges. A direct edge always wins; when none
// exists and the request does not involve the canonical hub, the conversion is
// routed through PTPN (input -> PTPN -> target).
class ConversionRegistry {
 public:
  void add(std::unique_ptr<Converter> converter);

  [[nodiscard]] ConversionResult convert(const Model& input, ModelKind target,
                                         const ConversionOptions& options) const;

  [[nodiscard]] bool has_direct(ModelKind from, ModelKind to) const;

 private:
  [[nodiscard]] const Converter* find_direct(ModelKind from, ModelKind to) const;

  std::vector<std::unique_ptr<Converter>> converters_;
};

// Registry preloaded with every supported edge.
ConversionRegistry make_default_registry();

}  // namespace lowering

#endif  // LOWER_CONVERT_CONVERSION_H
