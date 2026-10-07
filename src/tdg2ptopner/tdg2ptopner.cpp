#include "tdg2ptopner.h"

#include <spdlog/spdlog.h>

#include "export_ppn.h"
#include "ptpn_to_ppn.h"
#include "tdg2pn/tdg2pn.h"

namespace ptopner_export {

Tdg2PtopnerResult export_ptpn_to_ppn_file(const petri::PTPN& ptpn, const std::string& output_path) {
  Tdg2PtopnerResult result;
  try {
    const PpnModel model = ptpn_to_ppn_model(ptpn);
    if (!export_ppn(model, output_path)) {
      result.error_message = "Failed to write .ppn file: " + output_path;
      return result;
    }

    spdlog::info("[TDG2PTOPNER] Exported {} places, {} transitions to {}", model.places.size(),
                 model.transitions.size(), output_path);
    result.success = true;
  } catch (const std::exception& e) {
    result.error_message = e.what();
  }
  return result;
}

Tdg2PtopnerResult transform_to_ppn_file(const tdg::TDG& tdg, const std::string& output_path) {
  Tdg2PtopnerResult result;
  result.validation = validate_for_ptopner(tdg);
  if (!result.validation.ok) {
    result.error_message = "PToPNer validation failed";
    return result;
  }

  for (const auto& warning : result.validation.warnings) {
    spdlog::warn("[TDG2PTOPNER] {}", warning);
  }

  try {
    petri::PTPN ptpn;
    converter::TDG2PN::transform(tdg, ptpn);
    Tdg2PtopnerResult export_result = export_ptpn_to_ppn_file(ptpn, output_path);
    export_result.validation = result.validation;
    return export_result;
  } catch (const std::exception& e) {
    result.error_message = e.what();
    return result;
  }
}

}  // namespace ptopner_export
