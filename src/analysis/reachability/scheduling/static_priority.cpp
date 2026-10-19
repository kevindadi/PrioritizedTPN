#include "analysis/reachability/scheduling/static_priority.h"

namespace state_class {

int StaticPriorityRule::effective_priority(const PolicyContext& context, size_t transition) const {
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool StaticPriorityRule::admissible(const PolicyContext& /*context*/, size_t /*transition*/) const {
  return true;
}
}  // namespace state_class
