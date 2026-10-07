#ifndef TDG2PTOPNER_H
#define TDG2PTOPNER_H

#include <string>

#include "model/petri.h"
#include "model/tdg.h"
#include "validate.h"

namespace ptopner_export {

struct Tdg2PtopnerResult {
  bool success = false;
  PtopnerValidationResult validation;
  std::string error_message;
};

// Exports an already-built PTPN to a PToPNer .ppn file.
Tdg2PtopnerResult export_ptpn_to_ppn_file(const petri::PTPN& ptpn, const std::string& output_path);

// Validates TDG input, lowers to PTPN, and writes a .ppn file.
Tdg2PtopnerResult transform_to_ppn_file(const tdg::TDG& tdg, const std::string& output_path);

}  // namespace ptopner_export

#endif  // TDG2PTOPNER_H
