#include "export/export_dot.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <limits>
#include <spdlog/spdlog.h>
#include <sstream>

namespace petri::exporting {

namespace {

bool is_helper_transition(const ExportTransition& transition) {
  return transition.core < 0;
}

bool is_immediate_transition(const ExportTransition& transition) {
  return transition.earliest == 0 && transition.latest == 0;
}

bool should_show_scheduling_meta(const ExportTransition& transition) {
  if (transition.priority == 0 && transition.core == petri::kControlTransitionCore) {
    return false;
  }
  if (is_helper_transition(transition)) {
    return false;
  }
  return true;
}

std::string escape_dot_string(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

std::string quote_dot_string(const std::string& value) {
  return "\"" + escape_dot_string(value) + "\"";
}

std::string format_int_or_infinity(int value) {
  return value == std::numeric_limits<int>::max() ? "∞" : std::to_string(value);
}

std::string format_place_label(const ExportPlace& place) {
  std::string label = place.name;
  if (place.kind != PlaceKind::NORMAL || place.initial_tokens > 0 || place.capacity != 1) {
    label += "\nM=" + std::to_string(place.initial_tokens) +
             ", C=" + format_int_or_infinity(place.capacity);
  }
  return label;
}

std::string format_transition_label(const ExportTransition& transition) {
  std::string label = transition.name;

  // Always show the (core, priority) pair so the scheduling intent of every
  // transition (including control transitions) is visible at a glance.
  label += "\n(core=" + std::to_string(transition.core) +
           ", π=" + std::to_string(transition.priority) + ")";

  label += "\nI=[" + std::string(transition.left_open ? "(" : "[") +
           format_int_or_infinity(transition.earliest) + ", " +
           format_int_or_infinity(transition.latest) +
           std::string(transition.right_open ? ")" : "]");
  return label;
}

std::string place_fillcolor(const ExportPlace& place) {
  switch (place.kind) {
    case PlaceKind::CPU_RESOURCE:
      return "#dbeafe";
    case PlaceKind::LOCK_RESOURCE:
      return "#fef3c7";
    case PlaceKind::NORMAL:
    default:
      return "#ffffff";
  }
}

std::string place_color(const ExportPlace& place) {
  switch (place.kind) {
    case PlaceKind::CPU_RESOURCE:
      return "#2563eb";
    case PlaceKind::LOCK_RESOURCE:
      return "#d97706";
    case PlaceKind::NORMAL:
    default:
      return "#374151";
  }
}

std::string transition_fillcolor(const ExportTransition& transition) {
  if (transition.suspendable) {
    return "#fce7f3";
  }
  if (is_immediate_transition(transition) && should_show_scheduling_meta(transition)) {
    return "#fde68a";
  }
  return "#e5e7eb";
}

std::string transition_color(const ExportTransition& transition) {
  if (transition.suspendable) {
    return "#be185d";
  }
  if (is_immediate_transition(transition) && should_show_scheduling_meta(transition)) {
    return "#d97706";
  }
  return "#6b7280";
}

std::string node_id(const PetriExportModel& model, const ExportNodeRef& ref) {
  if (ref.kind == NodeKind::PLACE) {
    return model.places.at(ref.index).id;
  }
  return model.transitions.at(ref.index).id;
}

}  // namespace

std::string render_dot(const PetriExportModel& model) {
  std::ostringstream out;
  out << "digraph G {\n";
  out << "graph [rankdir=LR, fontname=\"Helvetica\", nodesep=0.35, "
         "ranksep=0.55, bgcolor=\"white\"];\n";
  out << "node [fontname=\"Helvetica\", margin=0.08, style=\"filled,rounded\", "
         "fontcolor=\"#111827\"];\n";
  out << "edge [fontname=\"Helvetica\", color=\"#9ca3af\", arrowsize=0.7];\n";

  for (const auto& place : model.places) {
    out << quote_dot_string(place.id) << " ["
        << "label=" << quote_dot_string(format_place_label(place)) << ", shape=\"circle\""
        << ", style=\"filled,rounded\""
        << ", fillcolor=" << quote_dot_string(place_fillcolor(place))
        << ", color=" << quote_dot_string(place_color(place)) << ", fontcolor=\"#111827\""
        << ", penwidth=\"1.4\""
        << "];\n";
  }

  for (const auto& transition : model.transitions) {
    out << quote_dot_string(transition.id) << " ["
        << "label=" << quote_dot_string(format_transition_label(transition)) << ", shape=\"box\""
        << ", style=\"filled,rounded\""
        << ", fillcolor=" << quote_dot_string(transition_fillcolor(transition))
        << ", color=" << quote_dot_string(transition_color(transition)) << ", fontcolor=\"#111827\""
        << ", penwidth=" << quote_dot_string(transition.suspendable ? "2.2" : "1.4") << "];\n";
  }

  for (const auto& arc : model.arcs) {
    out << quote_dot_string(node_id(model, arc.source)) << " -> "
        << quote_dot_string(node_id(model, arc.target)) << " ["
        << "label=" << quote_dot_string(arc.weight > 1 ? std::to_string(arc.weight) : "")
        << ", color=\"#9ca3af\""
        << ", penwidth=" << quote_dot_string(arc.weight > 1 ? "1.6" : "1.0") << "];\n";
  }

  out << "}\n";
  return out.str();
}

bool save_to_dot(const PetriExportModel& model, const std::string& file_path) {
  try {
    boost::filesystem::path dot_filename(file_path);
    if (!dot_filename.parent_path().empty() &&
        !boost::filesystem::exists(dot_filename.parent_path())) {
      boost::filesystem::create_directories(dot_filename.parent_path());
    }

    std::ofstream ofs(dot_filename.string());
    if (!ofs) {
      spdlog::error("[PETRI_EXPORT] Cannot open DOT file: {}", dot_filename.string());
      return false;
    }

    ofs << render_dot(model);
    ofs.close();

    spdlog::info("[PETRI_EXPORT] DOT file saved to: {}",
                 boost::filesystem::absolute(dot_filename).string());
    return true;
  } catch (const std::exception& e) {
    spdlog::error("[PETRI_EXPORT] Error saving DOT file: {}", e.what());
    return false;
  }
}

}  // namespace petri::exporting
