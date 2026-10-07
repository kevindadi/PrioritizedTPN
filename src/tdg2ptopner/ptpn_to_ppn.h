#ifndef PTPN_TO_PPN_H
#define PTPN_TO_PPN_H

#include "petri/petri.h"
#include "ppn_model.h"

namespace ptopner_export {

enum class TransitionRole {
  CONTROL,
  GET_CORE,
  EXEC,
  PREEMPT,
};

TransitionRole classify_transition(const std::string& name);
float map_prior_to_float(int ptpn_priority, TransitionRole role);
PpnModel ptpn_to_ppn_model(const petri::PTPN& ptpn);

}  // namespace ptopner_export

#endif  // PTPN_TO_PPN_H
