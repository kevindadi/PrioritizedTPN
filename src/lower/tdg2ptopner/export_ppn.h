#ifndef EXPORT_PPN_H
#define EXPORT_PPN_H

#include <string>

#include "ppn_model.h"

namespace ptopner_export {

bool export_ppn(const PpnModel& model, const std::string& path);
std::string ppn_to_string(const PpnModel& model);

}  // namespace ptopner_export

#endif  // EXPORT_PPN_H
