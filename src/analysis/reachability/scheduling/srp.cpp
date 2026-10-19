#include "analysis/reachability/scheduling/srp.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "analysis/reachability/scheduling/policy_common.h"
#include "common/unsupported_class.h"

namespace state_class {

SrpFiringRule::SrpFiringRule(const petri::PTPN& net) {
  index_ = std::make_unique<detail::TaskLockTopology>();
  index_->build(net);
}

void SrpFiringRule::ensure_index(const petri::PTPN& net) const {
  if (!index_ || !index_->built_for(net)) {
    index_ = std::make_unique<detail::TaskLockTopology>();
    index_->build(net);
    cache_valid_ = false;
  }
}

std::optional<int> SrpFiringRule::lock_ceiling(const std::string& lock) const {
  if (!index_ || index_->net() == nullptr) {
    return std::nullopt;
  }
  return index_->lock_ceiling(lock);
}

const detail::TaskLockTopology::MarkingState& SrpFiringRule::dynamic_state(
    const petri::PTPN& net, const petri::Marking& marking) const {
  ensure_index(net);

  if (cache_valid_ && cached_net_ == &net && cached_marking_ == marking) {
    return cached_layout_;
  }

  cached_layout_ = index_->analyze_marking(marking);
  cached_marking_ = marking;
  cached_net_ = &net;
  cache_valid_ = true;
  return cached_layout_;
}

int SrpFiringRule::effective_priority(const PolicyContext& context, size_t transition) const {
  // SRP has no priority inheritance: every transition keeps its statically
  // encoded priority. For task chains this equals the task's base priority;
  // control and non-task transitions keep their static priority.
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool SrpFiringRule::admissible(const PolicyContext& context, size_t transition) const {
  if (context.state == nullptr) {
    return true;  // no marking: no ceiling information, no restriction
  }

  ensure_index(context.net);
  const size_t num_transitions = index_->task_of_transition().size();
  if (transition >= num_transitions) {
    return true;  // control/other transition
  }
  const int task = index_->task_of_transition()[transition];
  if (task < 0) {
    return true;  // control/other transition
  }
  const detail::TaskLockTopology::TaskRecord& record = index_->tasks()[static_cast<size_t>(task)];

  // Only the get_core transition (chain position 1) opens a task. Every other
  // transition is admissible regardless of the system ceiling.
  if (record.chain.size() < 2 || !record.chain[1].is_transition() ||
      record.chain[1].as_transition().index() != transition) {
    return true;
  }

  const petri::Marking& marking = context.state->marking;
  if (marking.size() != context.net.num_places()) {
    return true;  // defensive fallback: cannot reason about this marking
  }

  const detail::TaskLockTopology::MarkingState& layout = dynamic_state(context.net, marking);
  const size_t ordinal = static_cast<size_t>(task);
  if (ordinal >= layout.supported.size() || !layout.supported[ordinal]) {
    // Unsupported/ambiguous chain: skip the admission check. This is a
    // defensive fallback, not a certified SRP-exact state.
    return true;
  }
  if (layout.token_position[ordinal] != 0) {
    return true;  // the task already started; SRP never blocks it again
  }

  const std::optional<int> ceiling = index_->system_ceiling(layout);
  if (!ceiling) {
    return true;  // no lock held: empty system ceiling
  }
  return record.base_priority > *ceiling;
}

}  // namespace state_class
