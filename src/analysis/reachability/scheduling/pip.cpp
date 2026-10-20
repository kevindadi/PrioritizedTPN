#include "analysis/reachability/scheduling/pip.h"

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

PipFiringRule::PipFiringRule(const petri::PTPN& net) {
  index_ = std::make_unique<detail::TaskLockTopology>();
  index_->build(net);
}

void PipFiringRule::ensure_index(const petri::PTPN& net) const {
  if (!index_ || !index_->built_for(net)) {
    index_ = std::make_unique<detail::TaskLockTopology>();
    index_->build(net);
    cache_valid_ = false;
  }
}

const std::vector<int>& PipFiringRule::effective_priorities(const petri::PTPN& net,
                                                            const petri::Marking& marking) const {
  ensure_index(net);

  if (cache_valid_ && cached_net_ == &net && cached_marking_ == marking) {
    return cached_effective_;
  }

  const detail::TaskLockTopology& topology = *index_;
  const size_t task_count = topology.tasks().size();
  std::vector<int> effective(task_count, 0);
  for (size_t i = 0; i < task_count; ++i) {
    effective[i] = topology.tasks()[i].base_priority;
  }

  const detail::TaskLockTopology::MarkingState layout = topology.analyze_marking(marking);
  std::vector<std::pair<int, int>> wait_edges;
  for (const auto& [waiter, lock] : layout.blocked_requests) {
    const auto holder_it = layout.unique_holder.find(lock);
    if (holder_it == layout.unique_holder.end() || holder_it->second == waiter) {
      continue;
    }
    wait_edges.emplace_back(waiter, holder_it->second);
  }
  detail::inherit_priorities(effective, wait_edges, task_count);

  cached_effective_ = std::move(effective);
  cached_marking_ = marking;
  cached_net_ = &net;
  cache_valid_ = true;
  return cached_effective_;
}

int PipFiringRule::effective_priority(const PolicyContext& context, size_t transition) const {
  ensure_index(context.net);
  const size_t num_transitions = index_->task_of_transition().size();
  if (transition < num_transitions) {
    const int task = index_->task_of_transition()[transition];
    if (task >= 0 && context.state != nullptr) {
      const std::vector<int>& effective = effective_priorities(context.net, context.state->marking);
      if (static_cast<size_t>(task) < effective.size()) {
        return effective[static_cast<size_t>(task)];
      }
    }
  }
  // Control transitions and transitions outside every task chain keep their
  // statically encoded priority.
  return context.net.get_transition(petri::TransitionId{transition}).priority;
}

bool PipFiringRule::admissible(const PolicyContext& /*context*/, size_t /*transition*/) const {
  // PIP only changes effective priorities; it never blocks a transition.
  return true;
}

}  // namespace state_class
