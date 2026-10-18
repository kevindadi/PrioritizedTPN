#include "analysis/reachability/scheduling_policy.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>

#include "analysis/reachability/state_class.h"

namespace state_class {

// ---------------------------------------------------------------------------
// Static rule
// ---------------------------------------------------------------------------

bool FiringRule::admissible(const PolicyContext& /*context*/,
                            size_t /*transition*/) const {
  return true;
}

int StaticPriorityRule::effective_priority(const PolicyContext& context,
                                           size_t transition) const {
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool StaticPriorityRule::admissible(const PolicyContext& /*context*/,
                                    size_t /*transition*/) const {
  return true;
}

// ---------------------------------------------------------------------------
// Policy -> rule mapping
// ---------------------------------------------------------------------------

std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy) {
  switch (policy) {
    case SchedulePolicy::FIXED:
    case SchedulePolicy::FIXED_PRIOR_WITH_RESUME:
    case SchedulePolicy::RM:
    case SchedulePolicy::DM:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::RESUME);
    case SchedulePolicy::FIXED_PRIOR_WITH_RESTART:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::RESTART);
    case SchedulePolicy::PIP:
    case SchedulePolicy::PCP:
    case SchedulePolicy::SRP:
    case SchedulePolicy::EDF:
    case SchedulePolicy::LLF:
    case SchedulePolicy::FIFO:
    case SchedulePolicy::UNKNOWN:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::NON_PREEMPTIVE);
  }
  return std::make_unique<StaticPriorityRule>(PreemptionMode::NON_PREEMPTIVE);
}

}  // namespace state_class
