#include "analysis/reachability/scheduling/edf.h"
#include "analysis/reachability/scheduling/fifo.h"
#include "analysis/reachability/scheduling/llf.h"
#include "analysis/reachability/scheduling/pcp.h"
#include "analysis/reachability/scheduling/pip.h"
#include "analysis/reachability/scheduling/srp.h"
#include "analysis/reachability/scheduling/static_priority.h"

#include <memory>

namespace state_class {

std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy) {
  switch (policy) {
    case SchedulePolicy::FIXED:
    case SchedulePolicy::FIXED_PRIOR_WITH_RESUME:
    case SchedulePolicy::RM:
    case SchedulePolicy::DM:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::RESUME);
    case SchedulePolicy::PIP:
      // Net-aware behavior is installed lazily from PolicyContext::net.
      return std::make_unique<PipFiringRule>();
    case SchedulePolicy::PCP:
      // Net-aware behavior is installed lazily from PolicyContext::net.
      return std::make_unique<PcpFiringRule>();
    case SchedulePolicy::SRP:
      // Net-aware behavior is installed lazily from PolicyContext::net.
      return std::make_unique<SrpFiringRule>();
    case SchedulePolicy::FIXED_PRIOR_WITH_RESTART:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::RESTART);
    case SchedulePolicy::EDF:
      // Net-aware behavior is installed lazily from PolicyContext::net; the
      // release-clock metadata is validated on first index build.
      return std::make_unique<EdfFiringRule>();
    case SchedulePolicy::FIFO:
      // Arrival clocks and non-preemptive branches; validated on first index
      // build from PolicyContext::net.
      return std::make_unique<FifoFiringRule>();
    case SchedulePolicy::LLF:
      // Event-driven LLF: laxity clocks and maximum-chi branches; validated on
      // first index build from PolicyContext::net. Never falls back to static
      // priorities.
      return std::make_unique<LlfFiringRule>();
    case SchedulePolicy::UNKNOWN:
      return std::make_unique<StaticPriorityRule>(PreemptionMode::NON_PREEMPTIVE);
  }
  return std::make_unique<StaticPriorityRule>(PreemptionMode::NON_PREEMPTIVE);
}

std::unique_ptr<FiringRule> make_firing_rule(SchedulePolicy policy, const petri::PTPN& net) {
  if (policy == SchedulePolicy::EDF) {
    return std::make_unique<EdfFiringRule>(net);
  }
  if (policy == SchedulePolicy::FIFO) {
    return std::make_unique<FifoFiringRule>(net);
  }
  if (policy == SchedulePolicy::LLF) {
    return std::make_unique<LlfFiringRule>(net);
  }
  if (policy == SchedulePolicy::PIP) {
    return std::make_unique<PipFiringRule>(net);
  }
  if (policy == SchedulePolicy::PCP) {
    return std::make_unique<PcpFiringRule>(net);
  }
  if (policy == SchedulePolicy::SRP) {
    return std::make_unique<SrpFiringRule>(net);
  }
  return make_firing_rule(policy);
}

}  // namespace state_class
