#include "analysis/ptpn_analysis.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <spdlog/spdlog.h>
#include <sstream>

#include "analysis/clock_state.h"
#include "analysis/scheduling.h"

namespace state_class {

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
      default:
        out += ch;
        break;
    }
  }
  return out;
}

}  // namespace

StateClassReachabilityGraph::StateClassReachabilityGraph(const petri::PTPN& net) : net_(net) {}

void StateClassReachabilityGraph::set_canonicalization_mode(CanonicalizationMode mode) {
  mode_ = mode;
}

void StateClassReachabilityGraph::set_extrapolation(bool enabled) {
  extrapolation_enabled_ = enabled;
  if (!enabled) {
    return;
  }
  // Uniform bound k: the largest finite endpoint of any static interval. Any
  // clock value beyond k can never influence a future firing decision.
  int k = 0;
  for (size_t t = 0; t < net_.num_transitions(); ++t) {
    k = std::max(k, effective_earliest(t));
    const int latest = effective_latest(t);
    if (latest != INF_TIME) {
      k = std::max(k, latest);
    }
  }
  extrapolation_k_ = k;
}

CanonicalizationMode StateClassReachabilityGraph::get_canonicalization_mode() const {
  return mode_;
}

int StateClassReachabilityGraph::effective_earliest(size_t transition) const {
  return net_.get_transition(transition).time_interval.effective_earliest();
}

int StateClassReachabilityGraph::effective_latest(size_t transition) const {
  const int latest = net_.get_transition(transition).time_interval.effective_latest();
  return latest == petri::INF ? INF_TIME : latest;
}

void StateClassReachabilityGraph::recompute_sets(StateClass& state) const {
  state.struct_enabled = Scheduling::structural_enabled(net_, state.marking);
  state.priority_enabled = Scheduling::filter_priority_per_core(state.struct_enabled, net_);

  state.suspended.clear();
  // struct_enabled is sorted, so suspended stays sorted too.
  for (size_t t : state.struct_enabled) {
    if (contains(state.priority_enabled, t)) {
      continue;
    }
    if (net_.get_transition(t).suspendable) {
      state.suspended.push_back(t);
    }
  }
}

void StateClassReachabilityGraph::build_layout(StateClass& state) const {
  const size_t num_transitions = net_.num_transitions();

  state.clock_vars.clear();
  state.clock_vars.push_back({ClockKind::Zero, NO_TRANSITION});
  state.exec_clock_of_transition.assign(num_transitions, -1);
  state.susp_clock_of_transition.assign(num_transitions, -1);

  for (size_t t : state.struct_enabled) {
    const int exec_idx = static_cast<int>(state.clock_vars.size());
    state.clock_vars.push_back({ClockKind::Execution, t});
    state.exec_clock_of_transition[t] = exec_idx;

    if (contains(state.suspended, t)) {
      const int susp_idx = static_cast<int>(state.clock_vars.size());
      state.clock_vars.push_back({ClockKind::Suspension, t});
      state.susp_clock_of_transition[t] = susp_idx;
    }
  }
}

StateClass StateClassReachabilityGraph::compute_initial_class() {
  StateClass state;
  state.marking = net_.get_marking();
  state.elapsed_time = 0.0;

  recompute_sets(state);
  build_layout(state);

  // Every clock starts at zero, i.e. pinned to the reference variable x0.
  const size_t n = state.clock_vars.size();
  DBM zone(n);
  for (size_t i = 1; i < n; ++i) {
    zone.set_constraint(0, i, 0);
    zone.set_constraint(i, 0, 0);
  }
  zone.minimize();
  state.zone = std::move(zone);

  return state;
}

StateClass StateClassReachabilityGraph::time_elapse(const StateClass& state) const {
  StateClass out = state;
  const size_t n = out.zone.size();
  if (n == 0) {
    return out;
  }

  // Running clocks advance at rate 1; everything else (x0 and the frozen
  // execution clocks of priority-filtered-out transitions) stays put.
  // V_run = { h_t : t in E_pri } U { w_t : t in suspended }.
  std::vector<bool> running(n, false);
  for (size_t i = 1; i < n && i < out.clock_vars.size(); ++i) {
    const ClockVar& var = out.clock_vars[i];
    if (var.kind == ClockKind::Suspension) {
      running[i] = true;  // suspension clocks always advance
    } else if (var.kind == ClockKind::Execution && contains(out.priority_enabled, var.transition)) {
      running[i] = true;  // active execution clocks advance
    }
  }

  // Release each running clock's upper bound relative to every stationary
  // variable (x0 and the frozen clocks). The differences between two running
  // clocks are left untouched, so they keep growing synchronously.
  for (size_t i = 1; i < n; ++i) {
    if (!running[i]) {
      continue;
    }
    for (size_t j = 0; j < n; ++j) {
      if (j != i && !running[j]) {
        out.zone.set_constraint(i, j, INF_TIME);
      }
    }
  }

  // Strong time semantics: cap each active execution clock at its deadline
  // upSI(t) so time cannot advance past a transition that must fire.
  for (size_t t : out.priority_enabled) {
    if (!out.has_exec_clock(t)) {
      continue;
    }
    const int upper = effective_latest(t);
    if (upper == INF_TIME) {
      continue;
    }
    const size_t idx = static_cast<size_t>(out.exec_index(t));
    const int current = out.zone.get_constraint(idx, 0);
    if (current == INF_TIME || upper < current) {
      out.zone.set_constraint(idx, 0, upper);
    }
  }

  out.zone.minimize();
  return out;
}

bool StateClassReachabilityGraph::is_firable(const StateClass& elapsed, size_t t) const {
  if (!contains(elapsed.priority_enabled, t) || !elapsed.has_exec_clock(t)) {
    return false;
  }

  const size_t idx = static_cast<size_t>(elapsed.exec_index(t));
  const int max_h = elapsed.zone.get_constraint(idx, 0);  // largest feasible h_t
  if (max_h == INF_TIME) {
    return true;
  }
  return max_h >= effective_earliest(t);
}

void StateClassReachabilityGraph::build_successor_zone(StateClass& successor, const DBM& fired,
                                                       const StateClass& source,
                                                       size_t fired_transition) const {
  const size_t n = successor.clock_vars.size();
  DBM zone(n);

  // For each successor variable, find the matching column in `fired` if the
  // clock survives the firing (a persistent execution clock keeps its value; a
  // still-suspended clock inherits its suspension clock). Everything else is
  // freshly created and starts at zero.
  std::vector<int> source_index(n, -1);
  source_index[0] = 0;  // x0 maps to x0
  for (size_t i = 1; i < n; ++i) {
    const ClockVar& var = successor.clock_vars[i];
    const size_t t = var.transition;
    if (var.kind == ClockKind::Execution) {
      if (t != fired_transition && source.has_exec_clock(t)) {
        source_index[i] = source.exec_index(t);
      }
    } else {  // Suspension
      if (t != fired_transition && source.has_susp_clock(t)) {
        source_index[i] = source.susp_index(t);
      }
    }
  }

  // Carry over the joint constraints between all surviving clocks.
  for (size_t i = 0; i < n; ++i) {
    if (source_index[i] < 0) {
      continue;
    }
    for (size_t j = 0; j < n; ++j) {
      if (source_index[j] < 0) {
        continue;
      }
      zone.set_constraint(i, j,
                          fired.get_constraint(static_cast<size_t>(source_index[i]),
                                               static_cast<size_t>(source_index[j])));
    }
  }

  // Pin every freshly created clock to zero (equal to x0).
  for (size_t i = 1; i < n; ++i) {
    if (source_index[i] < 0) {
      zone.set_constraint(0, i, 0);
      zone.set_constraint(i, 0, 0);
    }
  }

  zone.minimize();
  successor.zone = std::move(zone);
}

bool StateClassReachabilityGraph::fire(const StateClass& elapsed, size_t t,
                                       StateClass& successor) const {
  if (!is_firable(elapsed, t)) {
    return false;
  }

  // Step 1: intersect the firing-domain constraint h_t >= downSI(t). The
  // elapsed zone is already canonical, so a single-constraint incremental
  // tightening (O(n^2)) replaces the full closure.
  DBM fired = elapsed.zone;
  const size_t idx = static_cast<size_t>(elapsed.exec_index(t));
  const int lower = effective_earliest(t);
  if (!fired.tighten(0, idx, -lower)) {
    return false;
  }

  // Step 2: discrete token shuffle.
  successor = StateClass();
  successor.marking = petri::PTPN::fire(elapsed.marking, net_, t);

  // Steps 3 & 4: recompute the scheduler sets and the variable layout, then
  // rebuild the zone carrying surviving clocks over from `fired`.
  recompute_sets(successor);
  build_layout(successor);
  build_successor_zone(successor, fired, elapsed, t);

  const int h_lower = -elapsed.zone.get_constraint(0, idx);
  const int firing_instant = std::max(lower, h_lower);
  successor.elapsed_time = elapsed.elapsed_time + std::max(0, firing_instant);

  return true;
}

bool StateClassReachabilityGraph::find_match(const StateClass& state, SCVertex& match) const {
  if (mode_ == CanonicalizationMode::EQUALITY) {
    auto it = vertices_by_hash_.find(hash_state_class(state));
    if (it == vertices_by_hash_.end()) {
      return false;
    }
    for (SCVertex candidate : it->second) {
      const StateClass& existing = boost::get(boost::vertex_name, graph_, candidate);
      if (check_equality(state, existing)) {
        match = candidate;
        return true;
      }
    }
    return false;
  }

  auto it = vertices_by_marking_.find(state.marking);
  if (it == vertices_by_marking_.end()) {
    return false;
  }
  for (SCVertex candidate : it->second) {
    const StateClass& existing = boost::get(boost::vertex_name, graph_, candidate);
    if (can_merge_into(state, existing, mode_)) {
      match = candidate;
      return true;
    }
  }
  return false;
}

SCVertex StateClassReachabilityGraph::add_state(StateClass state) {
  state.id = next_id_++;
  if (mode_ == CanonicalizationMode::EQUALITY) {
    const size_t hash = hash_state_class(state);
    SCVertex v = boost::add_vertex(std::move(state), graph_);
    vertices_by_hash_[hash].push_back(v);
    return v;
  }
  const std::vector<int> marking = state.marking;
  SCVertex v = boost::add_vertex(std::move(state), graph_);
  vertices_by_marking_[marking].push_back(v);
  return v;
}

size_t StateClassReachabilityGraph::build(size_t max_states) {
  graph_.clear();
  vertices_by_marking_.clear();
  vertices_by_hash_.clear();
  stats_ = Statistics();
  next_id_ = 0;
  reset_dbm_instrumentation();
  petri::reset_overflow_recording();

  StateClass initial = compute_initial_class();
  if (extrapolation_enabled_) {
    initial.zone.extrapolate(extrapolation_k_);
  }
  initial_vertex_ = add_state(std::move(initial));
  stats_.total_states = 1;

  std::vector<SCVertex> frontier{initial_vertex_};

  while (!frontier.empty()) {
    std::vector<SCVertex> next_frontier;

    for (SCVertex u : frontier) {
      if (stats_.total_states >= max_states) {
        stats_.truncated = true;
        break;
      }

      // Extract everything needed from the source state up front instead of
      // deep-copying the whole StateClass: with vecS vertex storage, the
      // reference returned by boost::get is invalidated by add_vertex, so no
      // reference into the graph may be held across add_state below.
      struct EntryBounds {
        bool has_clock = false;
        int low = 0;
        int high = 0;
      };

      std::vector<size_t> enabled;
      std::vector<EntryBounds> entry_bounds;
      StateClass elapsed;
      {
        const StateClass& current = boost::get(boost::vertex_name, graph_, u);
        elapsed = time_elapse(current);
        enabled.assign(current.priority_enabled.begin(), current.priority_enabled.end());
        entry_bounds.resize(enabled.size());
        // The entry range of h_t comes from the pre-elapse zone (same layout,
        // same index as the elapsed zone).
        for (size_t k = 0; k < enabled.size(); ++k) {
          const int hcidx = current.exec_index(enabled[k]);
          if (hcidx > 0) {
            const size_t cidx = static_cast<size_t>(hcidx);
            entry_bounds[k] = {true, -current.zone.get_constraint(0, cidx),
                               current.zone.get_constraint(cidx, 0)};
          }
        }
      }

      for (size_t k = 0; k < enabled.size(); ++k) {
        const size_t t = enabled[k];
        if (!is_firable(elapsed, t)) {
          continue;
        }

        StateClass successor;
        if (!fire(elapsed, t, successor)) {
          continue;
        }
        if (extrapolation_enabled_) {
          successor.zone.extrapolate(extrapolation_k_);
        }

        // The real firing window of h_t: intersect its feasible range in the
        // time-elapsed zone with the static interval [downSI, upSI].
        const size_t hidx = static_cast<size_t>(elapsed.exec_index(t));
        const int h_low = -elapsed.zone.get_constraint(0, hidx);
        const int h_high = elapsed.zone.get_constraint(hidx, 0);
        const int up = effective_latest(t);
        const int fire_min = std::max({0, effective_earliest(t), h_low});
        int fire_max = h_high;
        if (up != INF_TIME && (fire_max == INF_TIME || up < fire_max)) {
          fire_max = up;
        }

        // Global dwell in the source class before this firing. h_t advances at
        // rate 1 during the dwell, so dwell = fire_value - h_t(entry).
        int dwell_min = fire_min;
        int dwell_max = fire_max;
        if (entry_bounds[k].has_clock) {
          const int entry_low = entry_bounds[k].low;
          const int entry_high = entry_bounds[k].high;
          dwell_min = std::max(0, fire_min - entry_high);
          dwell_max = (fire_max == INF_TIME || entry_low == INF_TIME)
                          ? INF_TIME
                          : std::max(0, fire_max - entry_low);
        }
        FiringEdge edge(static_cast<int>(t), fire_min, fire_max);
        edge.dwell_min = dwell_min;
        edge.dwell_max = dwell_max;

        SCVertex v;
        if (find_match(successor, v)) {
          stats_.dedup_hits++;
          boost::add_edge(u, v, edge, graph_);
          stats_.total_transitions++;
          continue;
        }

        if (stats_.total_states >= max_states) {
          stats_.truncated = true;
          continue;
        }

        v = add_state(std::move(successor));
        stats_.total_states++;
        boost::add_edge(u, v, edge, graph_);
        stats_.total_transitions++;
        next_frontier.push_back(v);
      }

      if (stats_.truncated) {
        break;
      }
    }

    if (stats_.truncated) {
      break;
    }
    frontier = std::move(next_frontier);
  }

  spdlog::info(
      "[SCG] build complete: states={}, transitions={}, dedup_hits={}, "
      "truncated={}, dbm_minimize_calls={}",
      stats_.total_states, stats_.total_transitions, stats_.dedup_hits,
      stats_.truncated ? "true" : "false", get_dbm_instrumentation().minimize_calls);

  return stats_.total_states;
}

std::string StateClassReachabilityGraph::format_marking(const petri::PTPN& net,
                                                        const std::vector<int>& marking) {
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

std::string StateClassReachabilityGraph::format_transition_label(size_t transition_id) const {
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

std::string StateClassReachabilityGraph::format_transitions(
    const TransitionSet& transitions) const {
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

std::string StateClassReachabilityGraph::format_named_dbm(const StateClass& state) const {
  if (state.zone.size() == 0) {
    return "DBM(empty)";
  }

  std::vector<std::string> labels(state.zone.size());
  labels[0] = "x0";
  size_t cell_width = 6;
  for (size_t i = 1; i < state.zone.size() && i < state.clock_vars.size(); ++i) {
    const ClockVar& var = state.clock_vars[i];
    const std::string prefix = var.kind == ClockKind::Suspension ? "w" : "h";
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
      const std::string rendered = value == INF_TIME ? std::string("inf") : std::to_string(value);
      oss << " " << std::left << std::setw(static_cast<int>(cell_width)) << rendered << "|";
    }
    oss << "\n";
  }

  return oss.str();
}

std::string StateClassReachabilityGraph::format_state_dump(const StateClass& state) const {
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

std::vector<std::string> StateClassReachabilityGraph::format_zone_constraints(
    const StateClass& state, bool html) const {
  std::vector<std::string> lines;
  const size_t n = state.zone.size();
  if (n <= 1) {
    return lines;  // only the reference x0: no active clock
  }

  const std::string le = html ? "&le;" : "<=";
  const std::string ge = html ? "&ge;" : ">=";
  const std::string minus = html ? "&minus;" : "-";

  auto clock_name = [&](size_t i) {
    const ClockVar& var = state.clock_vars[i];
    const std::string prefix = var.kind == ClockKind::Suspension ? "w" : "h";
    return prefix + "(T" + std::to_string(var.transition) + ")";
  };

  // Per-clock bounds: -D[0][i] <= x_i <= D[i][0].
  for (size_t i = 1; i < n && i < state.clock_vars.size(); ++i) {
    const int upper = state.zone.get_constraint(i, 0);
    const int lower_raw = state.zone.get_constraint(0, i);
    const int lower = lower_raw == INF_TIME ? 0 : std::max(0, -lower_raw);  // clocks are >= 0
    const std::string name = clock_name(i);
    std::string body;
    if (upper != INF_TIME && upper == lower) {
      body = name + " = " + std::to_string(upper);
    } else if (upper == INF_TIME) {
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
      if (dij == INF_TIME) {
        continue;
      }
      const int upper_i = state.zone.get_constraint(i, 0);
      const int lower_j_raw = state.zone.get_constraint(0, j);
      const int lower_j = lower_j_raw == INF_TIME ? 0 : std::max(0, -lower_j_raw);
      const int implied = upper_i == INF_TIME ? INF_TIME : upper_i - lower_j;
      if (implied != INF_TIME && dij >= implied) {
        continue;  // already implied by the individual bounds
      }
      lines.push_back(clock_name(i) + " " + minus + " " + clock_name(j) + " " + le + " " +
                      std::to_string(dij));
    }
  }
  return lines;
}

std::string StateClassReachabilityGraph::format_state_label_html(const StateClass& state) const {
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

bool StateClassReachabilityGraph::save_to_dot(const std::string& file_path) const {
  std::ofstream out(file_path);
  if (!out.is_open()) {
    return false;
  }

  out << "digraph StateClassGraph {\n";
  out << "  rankdir=LR;\n";
  out << "  node [shape=box, fontname=\"Helvetica\", color=\"#111111\"];\n";
  out << "  edge [fontname=\"Helvetica\"];\n\n";

  // A state class is a symbolic set derived from time intervals, not a concrete
  // point on a global timeline. The node therefore shows the LOCAL clock zone
  // (DBM constraints), never a fixed absolute timestamp.
  typedef boost::graph_traits<SCGraph>::vertex_iterator VIt;
  VIt vi, vi_end;
  for (std::tie(vi, vi_end) = boost::vertices(graph_); vi != vi_end; ++vi) {
    const StateClass& state = boost::get(boost::vertex_name, graph_, *vi);
    out << "  s" << state.id << " [label=" << format_state_label_html(state) << ", tooltip=\""
        << escape_dot(format_state_dump(state)) << "\"];\n";
  }

  out << "\n";

  typedef boost::graph_traits<SCGraph>::edge_iterator EIt;
  EIt ei, ei_end;
  for (std::tie(ei, ei_end) = boost::edges(graph_); ei != ei_end; ++ei) {
    const SCVertex src = boost::source(*ei, graph_);
    const SCVertex tgt = boost::target(*ei, graph_);
    const FiringEdge& edge = boost::get(boost::edge_name, graph_, *ei);
    const StateClass& src_state = boost::get(boost::vertex_name, graph_, src);
    const StateClass& tgt_state = boost::get(boost::vertex_name, graph_, tgt);
    const std::string window =
        "[" + std::to_string(edge.firing_min) + ", " +
        (edge.firing_max == INF_TIME ? "inf" : std::to_string(edge.firing_max)) + "]";
    const std::string dwell =
        "[" + std::to_string(edge.dwell_min) + ", " +
        (edge.dwell_max == INF_TIME ? "inf" : std::to_string(edge.dwell_max)) + "]";
    const std::string label = format_transition_label(static_cast<size_t>(edge.transition_id)) +
                              "\\n@" + window + " dwell=" + dwell;
    out << "  s" << src_state.id << " -> s" << tgt_state.id << " [label=\"" << escape_dot(label)
        << "\"];\n";
  }

  out << "}\n";
  return true;
}

}  // namespace state_class
