#ifndef PETRI_EXPORT_PETRI_H
#define PETRI_EXPORT_PETRI_H

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "model/petri.h"

namespace petri::exporting {

enum class PlaceKind {
  NORMAL,
  CPU_RESOURCE,
  LOCK_RESOURCE,
};

enum class NodeKind {
  PLACE,
  TRANSITION,
};

struct ExportPlace {
  std::string id;
  std::string name;
  int initial_tokens = 0;
  int capacity = 1;
  PlaceKind kind = PlaceKind::NORMAL;
};

struct ExportTransition {
  std::string id;
  std::string name;
  int earliest = 0;
  int latest = std::numeric_limits<int>::max();
  bool left_open = false;
  bool right_open = false;
  int priority = std::numeric_limits<int>::max();
  int core = petri::kControlTransitionCore;
  bool suspendable = false;
};

struct ExportNodeRef {
  NodeKind kind = NodeKind::PLACE;
  size_t index = 0;
};

struct ExportArc {
  ExportNodeRef source;
  ExportNodeRef target;
  int weight = 1;
};

struct PetriExportModel {
  std::vector<ExportPlace> places;
  std::vector<ExportTransition> transitions;
  std::vector<ExportArc> arcs;
};

PetriExportModel build_export_model(const petri::PTPN& ptpn);

}  // namespace petri::exporting

#endif  // PETRI_EXPORT_PETRI_H
