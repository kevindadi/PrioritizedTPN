#ifndef TDG2ROMEO_H
#define TDG2ROMEO_H

#include <string>

#include "model/tdg.h"
#include "romeo_model.h"

namespace romeo {

enum class RomeoFormat { SchedulingNet, InhibitorArc };

struct RomeoExportOptions {
  RomeoFormat format = RomeoFormat::SchedulingNet;
  bool explicit_core_places = true;
};

struct RomeoExportResult {
  bool success = false;
  std::string error_message;
};

RomeoExportResult export_tdg_to_romeo_cts(const tdg::TDG& tdg, const std::string& output_path,
                                          const RomeoExportOptions& opts = {});

RomeoModel build_romeo_model(const tdg::TDG& tdg, const RomeoExportOptions& opts);

}  // namespace romeo

#endif  // TDG2ROMEO_H
