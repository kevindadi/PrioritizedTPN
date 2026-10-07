#ifndef TDG2PTOPNER_VALIDATE_H
#define TDG2PTOPNER_VALIDATE_H

#include <string>
#include <vector>

#include "tdg/tdg.h"

namespace ptopner_export {

struct PtopnerValidationResult {
  bool ok = true;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
};

PtopnerValidationResult validate_for_ptopner(const tdg::TDG& tdg);

}  // namespace ptopner_export

#endif  // TDG2PTOPNER_VALIDATE_H
