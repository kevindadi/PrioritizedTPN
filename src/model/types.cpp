#include "model/types.h"

SchedulePolicy parse_policy_string(const std::string& policy) {
  if (policy == "fixed") {
    return SchedulePolicy::FIXED;
  }
  if (policy == "fixed_prior_with_restart") {
    return SchedulePolicy::FIXED_PRIOR_WITH_RESTART;
  }
  if (policy == "fixed_prior_with_resume") {
    return SchedulePolicy::FIXED_PRIOR_WITH_RESUME;
  }
  if (policy == "rm") {
    return SchedulePolicy::RM;
  }
  if (policy == "dm") {
    return SchedulePolicy::DM;
  }
  if (policy == "edf") {
    return SchedulePolicy::EDF;
  }
  if (policy == "llf") {
    return SchedulePolicy::LLF;
  }
  if (policy == "fifo") {
    return SchedulePolicy::FIFO;
  }
  if (policy == "pip") {
    return SchedulePolicy::PIP;
  }
  if (policy == "pcp") {
    return SchedulePolicy::PCP;
  }
  if (policy == "srp") {
    return SchedulePolicy::SRP;
  }
  return SchedulePolicy::UNKNOWN;
}

std::string policy_to_string(SchedulePolicy policy) {
  switch (policy) {
    case SchedulePolicy::FIXED:
      return "fixed";
    case SchedulePolicy::FIXED_PRIOR_WITH_RESTART:
      return "fixed_prior_with_restart";
    case SchedulePolicy::FIXED_PRIOR_WITH_RESUME:
      return "fixed_prior_with_resume";
    case SchedulePolicy::RM:
      return "rm";
    case SchedulePolicy::DM:
      return "dm";
    case SchedulePolicy::EDF:
      return "edf";
    case SchedulePolicy::LLF:
      return "llf";
    case SchedulePolicy::FIFO:
      return "fifo";
    case SchedulePolicy::PIP:
      return "pip";
    case SchedulePolicy::PCP:
      return "pcp";
    case SchedulePolicy::SRP:
      return "srp";
    default:
      return "unknown";
  }
}

SchedulePolicy parse_schedule_policy(const std::string& policy) {
  return parse_policy_string(policy);
}

std::string schedule_policy_to_string(SchedulePolicy policy) {
  return policy_to_string(policy);
}