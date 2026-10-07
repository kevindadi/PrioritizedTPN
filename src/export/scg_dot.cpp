#include "export/scg_dot.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "analysis/clock_state.h"

namespace scg_export {

namespace {

std::string escape_dot(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

// Escapes the characters that are special inside Graphviz HTML-like labels.
std::string html_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

}  // namespace

ScgFormatter::ScgFormatter(const petri::PTPN& net, const state_class::SCGraph& graph)
    : net_(net), graph_(graph) {}

std::string ScgFormatter::format_marking(const petri::PTPN& net, const std::vector<int>& marking) {
  // Only list the marked places, referenced by name, so the dump stays
  // readable on large nets.
  std::string out = "[";
  bool first = true;
  for (size_t i = 0; i < marking.size(); ++i) {
    if (marking[i] <= 0) {
      continue;
    }
    if (!first) {
      out += ", ";
    }
    first = false;
    out += i < net.num_places() ? net.get_place(i).name : ("P" + std::to_string(i));
    if (marking[i] != 1) {
      out += "(" + std::to_string(marking[i]) + ")";
    }
  }
  if (first) {
    out += "empty";
  }
  out += "]";
  return out;
}

std::string ScgFormatter::format_transition_label(size_t transition_id) const {
  if (transition_id >= net_.num_transitions()) {
    return "T" + std::to_string(transition_id);
  }
  const auto& trans = net_.get_transition(transition_id);
  std::string out = "T" + std::to_string(transition_id) + "(" + trans.name;
  out += ", priority=" + std::to_string(trans.priority);
  out += ", core=" + std::to_string(trans.core);
  if (trans.suspendable) {
    out += ", suspendable";
  }
  out += ")";
  return out;
}

std::string ScgFormatter::format_transitions(const state_class::TransitionSet& transitions) const {
  if (transitions.empty()) {
    return "(none)";
  }
  std::string out;
  bool first = true;
  for (size_t t : transitions) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += format_transition_label(t);
  }
  return out;
}

std::string ScgFormatter::format_named_dbm(const state_class::StateClass& state) const {
  if (state.zone.size() == 0) {
    return "DBM(empty)";
  }

  std::vector<std::string> labels(state.zone.size());
  labels[0] = "x0";
  size_t cell_width = 6;
  for (size_t i = 1; i < state.zone.size() && i < state.clock_vars.size(); ++i) {
    const state_class::ClockVar& var = state.clock_vars[i];
    const std::string prefix = var.kind == state_class::ClockKind::Suspension ? "w" : "h";
    labels[i] = prefix + "(T" + std::to_string(var.transition) + ")";
    cell_width = std::max(cell_width, labels[i].size() + 2);
  }

  std::ostringstream oss;
  oss << "DBM(size=" << state.zone.size() << ")\n";
  oss << std::left << std::setw(static_cast<int>(cell_width)) << " " << "|";
  for (const auto& label : labels) {
    oss << " " << std::left << std::setw(static_cast<int>(cell_width)) << label << "|";
  }
  oss << "\n";

  for (size_t i = 0; i < state.zone.size(); ++i) {
    oss << std::left << std::setw(static_cast<int>(cell_width)) << labels[i] << "|";
    for (size_t j = 0; j < state.zone.size(); ++j) {
      const int value = state.zone.get_constraint(i, j);
      const std::string rendered =
          value == state_class::INF_TIME ? std::string("inf") : std::to_string(value);
      oss << " " << std::left << std::setw(static_cast<int>(cell_width)) << rendered << "|";
    }
    oss << "\n";
  }

  return oss.str();
}

std::string ScgFormatter::format_state_dump(const state_class::StateClass& state) const {
  std::ostringstream oss;
  oss << "State " << state.id << "\n";
  oss << "  Elapsed time: " << state.elapsed_time << "\n";
  oss << "  Marking: " << format_marking(net_, state.marking) << "\n";
  oss << "  E_struct: " << format_transitions(state.struct_enabled) << "\n";
  oss << "  E_pri (active): " << format_transitions(state.priority_enabled) << "\n";
  oss << "  Suspended: " << format_transitions(state.suspended) << "\n";
  oss << "  Zone:\n" << format_named_dbm(state);
  return oss.str();
}

std::vector<std::string> ScgFormatter::format_zone_constraints(const state_class::StateClass& state,
                                                               bool html) const {
  std::vector<std::string> lines;
  const size_t n = state.zone.size();
  if (n <= 1) {
    return lines;  // only the reference x0: no active clock
  }

  const std::string le = html ? "&le;" : "<=";
  const std::string ge = html ? "&ge;" : ">=";
  const std::string minus = html ? "&minus;" : "-";

  auto clock_name = [&](size_t i) {
    const state_class::ClockVar& var = state.clock_vars[i];
    const std::string prefix = var.kind == state_class::ClockKind::Suspension ? "w" : "h";
    return prefix + "(T" + std::to_string(var.transition) + ")";
  };

  // Per-clock bounds: -D[0][i] <= x_i <= D[i][0].
  for (size_t i = 1; i < n && i < state.clock_vars.size(); ++i) {
    const int upper = state.zone.get_constraint(i, 0);
    const int lower_raw = state.zone.get_constraint(0, i);
    const int lower = lower_raw == state_class::INF_TIME ? 0 : std::max(0, -lower_raw);
    const std::string name = clock_name(i);
    std::string body;
    if (upper != state_class::INF_TIME && upper == lower) {
      body = name + " = " + std::to_string(upper);
    } else if (upper == state_class::INF_TIME) {
      body = name + " " + ge + " " + std::to_string(lower);
    } else {
      body = std::to_string(lower) + " " + le + " " + name + " " + le + " " + std::to_string(upper);
    }
    lines.push_back(body);
  }

  // Non-trivial differences: only those tighter than what the per-clock bounds
  // already imply, so canonical zones stay uncluttered.
  for (size_t i = 1; i < n; ++i) {
    for (size_t j = 1; j < n; ++j) {
      if (i == j) {
        continue;
      }
      const int dij = state.zone.get_constraint(i, j);
      if (dij == state_class::INF_TIME) {
        continue;
      }
      const int upper_i = state.zone.get_constraint(i, 0);
      const int lower_j_raw = state.zone.get_constraint(0, j);
      const int lower_j = lower_j_raw == state_class::INF_TIME ? 0 : std::max(0, -lower_j_raw);
      const int implied =
          upper_i == state_class::INF_TIME ? state_class::INF_TIME : upper_i - lower_j;
      if (implied != state_class::INF_TIME && dij >= implied) {
        continue;  // already implied by the individual bounds
      }
      lines.push_back(clock_name(i) + " " + minus + " " + clock_name(j) + " " + le + " " +
                      std::to_string(dij));
    }
  }
  return lines;
}

std::string ScgFormatter::format_state_label_html(const state_class::StateClass& state) const {
  // Colour palette (readable on white, print friendly).
  constexpr const char* kIdentity = "#111111";   // state id / marking / enabled
  constexpr const char* kExecClock = "#1f6feb";  // h_t execution clocks (blue)
  constexpr const char* kSuspClock = "#e05d00";  // w_t suspension clocks (amber)
  constexpr const char* kDiff = "#6a737d";       // clock differences (grey)

  auto row = [](const std::string& color, const std::string& body, const char* align) {
    return "<tr><td align=\"" + std::string(align) + "\" balign=\"left\"><font color=\"" + color +
           "\">" + body + "</font></td></tr>";
  };

  std::string html =
      "<<table border=\"0\" cellborder=\"0\" cellspacing=\"0\" "
      "cellpadding=\"1\">";

  html += row(kIdentity, "<b>State " + std::to_string(state.id) + "</b>", "center");
  html += row(kIdentity, "M = " + html_escape(format_marking(net_, state.marking)), "left");
  html +=
      row(kIdentity, "E_pri: " + html_escape(format_transitions(state.priority_enabled)), "left");
  if (!state.suspended.empty()) {
    html += row(kIdentity, "susp: " + html_escape(format_transitions(state.suspended)), "left");
  }

  // Separator before the symbolic clock zone.
  html += "<hr/><tr><td align=\"center\"><font color=\"" + std::string(kIdentity) +
          "\"><i>clock zone</i></font></td></tr>";

  const std::vector<std::string> constraints = format_zone_constraints(state, /*html=*/true);
  if (constraints.empty()) {
    html += row(kDiff, "(no active clock)", "center");
  } else {
    for (const std::string& c : constraints) {
      const char* color = kDiff;  // clock differences stay grey
      if (c.find("&minus;") == std::string::npos) {
        if (c.find("w(") != std::string::npos) {
          color = kSuspClock;
        } else if (c.find("h(") != std::string::npos) {
          color = kExecClock;
        }
      }
      html += row(color, c, "left");
    }
  }

  html += "</table>>";
  return html;
}

std::string ScgFormatter::render_dot() const {
  std::ostringstream out;
  out << "digraph StateClassGraph {\n";
  out << "  rankdir=LR;\n";
  out << "  node [shape=box, fontname=\"Helvetica\", color=\"#111111\"];\n";
  out << "  edge [fontname=\"Helvetica\"];\n\n";

  // A state class is a symbolic set derived from time intervals, not a concrete
  // point on a global timeline. The node therefore shows the LOCAL clock zone
  // (DBM constraints), never a fixed absolute timestamp.
  typedef boost::graph_traits<state_class::SCGraph>::vertex_iterator VIt;
  VIt vi, vi_end;
  for (std::tie(vi, vi_end) = boost::vertices(graph_); vi != vi_end; ++vi) {
    const state_class::StateClass& state = boost::get(boost::vertex_name, graph_, *vi);
    out << "  s" << state.id << " [label=" << format_state_label_html(state) << ", tooltip=\""
        << escape_dot(format_state_dump(state)) << "\"];\n";
  }

  out << "\n";

  typedef boost::graph_traits<state_class::SCGraph>::edge_iterator EIt;
  EIt ei, ei_end;
  for (std::tie(ei, ei_end) = boost::edges(graph_); ei != ei_end; ++ei) {
    const state_class::SCVertex src = boost::source(*ei, graph_);
    const state_class::SCVertex tgt = boost::target(*ei, graph_);
    const state_class::FiringEdge& edge = boost::get(boost::edge_name, graph_, *ei);
    const state_class::StateClass& src_state = boost::get(boost::vertex_name, graph_, src);
    const state_class::StateClass& tgt_state = boost::get(boost::vertex_name, graph_, tgt);
    const std::string window =
        "[" + std::to_string(edge.firing_min) + ", " +
        (edge.firing_max == state_class::INF_TIME ? "inf" : std::to_string(edge.firing_max)) + "]";
    const std::string dwell =
        "[" + std::to_string(edge.dwell_min) + ", " +
        (edge.dwell_max == state_class::INF_TIME ? "inf" : std::to_string(edge.dwell_max)) + "]";
    const std::string label = format_transition_label(static_cast<size_t>(edge.transition_id)) +
                              "\\n@" + window + " dwell=" + dwell;
    out << "  s" << src_state.id << " -> s" << tgt_state.id << " [label=\"" << escape_dot(label)
        << "\"];\n";
  }

  out << "}\n";
  return out.str();
}

bool ScgFormatter::save_to_dot(const std::string& file_path) const {
  std::ofstream out(file_path);
  if (!out.is_open()) {
    return false;
  }
  out << render_dot();
  return true;
}

}  // namespace scg_export
