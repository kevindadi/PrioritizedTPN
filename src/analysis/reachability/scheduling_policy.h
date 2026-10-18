#ifndef ANALYSIS_REACHABILITY_SCHEDULING_POLICY_H
#define ANALYSIS_REACHABILITY_SCHEDULING_POLICY_H

#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "model/petri.h"

namespace state_class {

// `StateClass` is only referenced by pointer here (dynamic rules may read the
// marking/zone of the state being expanded). Keeping the forward declaration
// avoids an include cycle with state_class.h.
class StateClass;

// How a filtered-out transition is treated by the analysis engine.
enum class PreemptionMode {
  RESUME,         // freeze the execution clock; resume from the frozen value
  RESTART,        // structural restart encoding (see lowered preempt sub-net)
  NON_PREEMPTIVE  // no preemption; policy not otherwise implemented
};

// Inputs available to a firing rule when priorities/admissibility are computed.
struct PolicyContext {
  const petri::PTPN& net;
  // Marking is always set; the full clock layout/zone may be absent at the
  // point priority branches are computed. Static rules must not read it.
  const StateClass* state = nullptr;
};

// A scheduling policy expressed as a priority + admissibility function over
// the transitions of a PTPN. Higher `effective_priority` means higher
// scheduling priority. Implementations must be deterministic.
class FiringRule {
 public:
  virtual ~FiringRule() = default;

  virtual int effective_priority(const PolicyContext& context, size_t transition) const = 0;

  // Optional admissibility constraint (resource protocols, non-preemptive
  // running task). Default: every structurally enabled transition is
  // admissible.
  virtual bool admissible(const PolicyContext& context, size_t transition) const;

  virtual PreemptionMode preemption_mode() const = 0;
};

// Reads the priority statically encoded on each PTPN transition. This covers
// FIXED/FIXED_PRIOR_WITH_RESUME/FIXED_PRIOR_WITH_RESTART and RM/DM whose
// priorities are computed during lowering and stored on the transitions. The
// remaining policies (EDF/LLF/FIFO) keep their static priorities and are not
// otherwise analyzed yet.
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

// Policy -> rule mapping:
//   FIXED, FIXED_PRIOR_WITH_RESUME, RM, DM -> static priorities, RESUME
//   FIXED_PRIOR_WITH_RESTART              -> static priorities, RESTART
//   everything else                       -> static priorities, NON_PREEMPTIVE
std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy);

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_SCHEDULING_POLICY_H
