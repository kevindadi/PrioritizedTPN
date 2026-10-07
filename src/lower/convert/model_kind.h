#ifndef LOWER_CONVERT_MODEL_KIND_H
#define LOWER_CONVERT_MODEL_KIND_H

namespace lowering {

// The models that participate in the unified lowering/conversion framework.
enum class ModelKind {
  TDG,    // task dependency graph (source model)
  PTPN,   // priority timed Petri net (canonical net IR)
  ROMEO,  // Romeo scheduling/inhibitor net model
  PPN,    // PToPNer flat net model
};

inline const char* model_kind_name(ModelKind kind) {
  switch (kind) {
    case ModelKind::TDG:
      return "TDG";
    case ModelKind::PTPN:
      return "PTPN";
    case ModelKind::ROMEO:
      return "Romeo";
    case ModelKind::PPN:
      return "PToPNer";
  }
  return "unknown";
}

}  // namespace lowering

#endif  // LOWER_CONVERT_MODEL_KIND_H
