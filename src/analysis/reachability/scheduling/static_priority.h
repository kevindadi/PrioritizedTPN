#ifndef ANALYSIS_REACHABILITY_SCHEDULING_STATIC_PRIORITY_H
#define ANALYSIS_REACHABILITY_SCHEDULING_STATIC_PRIORITY_H

#include "analysis/reachability/scheduling/firing_rule.h"

namespace state_class {

// Reads the priority statically encoded on each PTPN transition. This covers
// FIXED/FIXED_PRIOR_WITH_RESUME/FIXED_PRIOR_WITH_RESTART and RM/DM whose
// priorities are computed during lowering and stored on the transitions. LLF
// and UNKNOWN keep their static priorities and are not otherwise analyzed yet.
class StaticPriorityRule : public FiringRule {
 public:
  explicit StaticPriorityRule(PreemptionMode mode) : mode_(mode) {}

  int effective_priority(const PolicyContext& context, size_t transition) const override;
  bool admissible(const PolicyContext& context, size_t transition) const override;

  PreemptionMode preemption_mode() const override {
    return mode_;
  }

 private:
  PreemptionMode mode_;
};

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_STATIC_PRIORITY_H
