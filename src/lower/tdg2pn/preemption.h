#ifndef LOWER_TDG2PN_PREEMPTION_H
#define LOWER_TDG2PN_PREEMPTION_H

#include <memory>

#include "model/petri.h"
#include "model/tdg.h"
#include "model/types.h"

namespace converter::detail {

// Policy-specific preemption encoding. New scheduling policies plug in here by
// implementing this interface and registering it in make_preemption_strategy().
class PreemptionStrategy {
 public:
  virtual ~PreemptionStrategy() = default;
  virtual void apply(petri::PTPN& ptpn, const tdg::TDG& tdg) const = 0;
};

std::unique_ptr<PreemptionStrategy> make_preemption_strategy(SchedulePolicy policy);

}  // namespace converter::detail

#endif  // LOWER_TDG2PN_PREEMPTION_H
