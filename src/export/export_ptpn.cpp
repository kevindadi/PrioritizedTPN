#include "export/export_ptpn.h"

#include <limits>
#include <spdlog/spdlog.h>

namespace petri::exporting {

namespace {

PlaceKind detect_place_kind(const std::string& name) {
  if (name.rfind("core", 0) == 0) {
    return PlaceKind::CPU_RESOURCE;
  }
  if (name.rfind("mutex", 0) == 0 || name.rfind("spin", 0) == 0) {
    return PlaceKind::LOCK_RESOURCE;
  }
  return PlaceKind::NORMAL;
}

}  // namespace

PetriExportModel build_export_model(const petri::PTPN& ptpn) {
  PetriExportModel model;
  const auto& marking = ptpn.get_marking();

  model.places.reserve(ptpn.num_places());
  for (size_t p = 0; p < ptpn.num_places(); ++p) {
    const auto& place = ptpn.get_place(petri::PlaceId{p});
    model.places.push_back({
        place.name,
        place.name,
        p < marking.size() ? marking[p] : 0,
        place.capacity,
        detect_place_kind(place.name),
    });
  }

  model.transitions.reserve(ptpn.num_transitions());
  for (size_t t = 0; t < ptpn.num_transitions(); ++t) {
    const auto& transition = ptpn.get_transition(petri::TransitionId{t});
    model.transitions.push_back({
        transition.name,
        transition.name,
        transition.time_interval.earliest,
        transition.time_interval.latest == petri::INF ? std::numeric_limits<int>::max()
                                                      : transition.time_interval.latest,
        transition.time_interval.left_open,
        transition.time_interval.right_open,
        transition.priority,
        transition.core,
        transition.suspendable,
    });
  }

  const auto& pre = ptpn.get_pre_matrix();
  for (size_t p = 0; p < pre.size(); ++p) {
    for (size_t t = 0; t < pre[p].size(); ++t) {
      if (pre[p][t] > 0) {
        model.arcs.push_back({{NodeKind::PLACE, p}, {NodeKind::TRANSITION, t}, pre[p][t]});
      }
    }
  }

  const auto& post = ptpn.get_post_matrix();
  for (size_t t = 0; t < post.size(); ++t) {
    for (size_t p = 0; p < post[t].size(); ++p) {
      if (post[t][p] > 0) {
        model.arcs.push_back({{NodeKind::TRANSITION, t}, {NodeKind::PLACE, p}, post[t][p]});
      }
    }
  }

  spdlog::info("[PETRI_EXPORT] Built export model: {} places, {} transitions, {} arcs",
               model.places.size(), model.transitions.size(), model.arcs.size());
  return model;
}

}  // namespace petri::exporting
