#ifndef PETRI_EXPORT_DOT_H
#define PETRI_EXPORT_DOT_H

#include <string>

#include "export/export_ptpn.h"

namespace petri::exporting {

std::string render_dot(const PetriExportModel& model);
bool save_to_dot(const PetriExportModel& model, const std::string& file_path);

}  // namespace petri::exporting

#endif  // PETRI_EXPORT_DOT_H
