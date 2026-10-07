#include "ptpn_to_ppn.h"

#include <stdexcept>

namespace ptopner_export {

namespace {

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& value, const std::string& needle) {
  return value.find(needle) != std::string::npos;
}

}  // namespace

TransitionRole classify_transition(const std::string& name) {
  if (ends_with(name, "get_core")) {
    return TransitionRole::GET_CORE;
  }
  if (contains(name, "restart_preempt_")) {
    return TransitionRole::PREEMPT;
  }
  if (contains(name, "exec")) {
    return TransitionRole::EXEC;
  }
  return TransitionRole::CONTROL;
}

float map_prior_to_float(int ptpn_priority, TransitionRole role) {
  switch (role) {
    case TransitionRole::CONTROL:
      return 0.0F;
    case TransitionRole::EXEC:
      return static_cast<float>(ptpn_priority);
    case TransitionRole::GET_CORE:
      return static_cast<float>(ptpn_priority) + 0.2F;
    case TransitionRole::PREEMPT:
      return static_cast<float>(ptpn_priority) + 0.1F;
  }
  return 0.0F;
}

PpnModel ptpn_to_ppn_model(const petri::PTPN& ptpn) {
  PpnModel model;
  const auto& marking = ptpn.get_marking();
  model.places.reserve(ptpn.num_places());
  for (size_t p = 0; p < ptpn.num_places(); ++p) {
    PpnPlace place;
    place.name = ptpn.get_place(p).name;
    place.token = p < marking.size() ? static_cast<short>(marking[p]) : 0;
    model.places.push_back(std::move(place));
  }

  const auto& pre = ptpn.get_pre_matrix();
  const auto& post = ptpn.get_post_matrix();
  model.transitions.reserve(ptpn.num_transitions());
  for (size_t t = 0; t < ptpn.num_transitions(); ++t) {
    const auto& transition = ptpn.get_transition(t);
    PpnTransition ppn_transition;
    ppn_transition.name = transition.name;
    ppn_transition.time = transition.time_interval.earliest;

    if (transition.time_interval.latest != transition.time_interval.earliest &&
        transition.time_interval.latest != petri::INF) {
      throw std::runtime_error("Transition " + transition.name + " has non-point time interval");
    }

    for (size_t p = 0; p < pre.size(); ++p) {
      if (p < pre[p].size() && pre[p][t] > 0) {
        ppn_transition.preset.push_back(static_cast<short>(p));
      }
    }
    for (size_t p = 0; p < post[t].size(); ++p) {
      if (post[t][p] > 0) {
        ppn_transition.postset.push_back(static_cast<short>(p));
      }
    }

    const TransitionRole role = classify_transition(transition.name);
    ppn_transition.prior = map_prior_to_float(transition.priority, role);
    ppn_transition.is_suspend = role == TransitionRole::EXEC && transition.suspendable;

    model.transitions.push_back(std::move(ppn_transition));
  }

  return model;
}

}  // namespace ptopner_export
