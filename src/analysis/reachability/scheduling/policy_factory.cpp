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
  std::unique_ptr<FiringRule> rule;
  if (policy == SchedulePolicy::EDF) {
    rule = std::make_unique<EdfFiringRule>(net);
  } else if (policy == SchedulePolicy::FIFO) {
    rule = std::make_unique<FifoFiringRule>(net);
  } else if (policy == SchedulePolicy::LLF) {
    rule = std::make_unique<LlfFiringRule>(net);
  } else if (policy == SchedulePolicy::PIP) {
    rule = std::make_unique<PipFiringRule>(net);
  } else if (policy == SchedulePolicy::PCP) {
    rule = std::make_unique<PcpFiringRule>(net);
  } else if (policy == SchedulePolicy::SRP) {
    rule = std::make_unique<SrpFiringRule>(net);
  } else {
    rule = make_firing_rule(policy);
  }
  // Scalar-priority rules honor the net's equal-priority tie handling; the
  // job-clock rules (EDF/FIFO/LLF) ignore it because their own branches()
  // resolves ties through clock constraints.
  rule->set_tie_mode(net.tie_mode());
  return rule;
}

}  // namespace state_class
