#include "export_ppn.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace ptopner_export {

namespace {

std::string format_arc_list(const std::vector<short>& arcs) {
  if (arcs.empty()) {
    return ".";
  }
  std::ostringstream oss;
  for (size_t i = 0; i < arcs.size(); ++i) {
    if (i > 0) {
      oss << ',';
    }
    oss << arcs[i];
  }
  oss << '.';
  return oss.str();
}

void write_transition_block(std::ostream& out, const PpnModel& model) {
  out << "transition  preset  postset  time  prior  is_suspend\n";
  for (size_t i = 0; i < model.transitions.size(); ++i) {
    const auto& transition = model.transitions[i];
    out << transition.name << "          " << format_arc_list(transition.preset) << "    "
        << format_arc_list(transition.postset) << "       " << transition.time << "    "
        << std::fixed << std::setprecision(1) << transition.prior << "      "
        << (transition.is_suspend ? 1 : 0);
    if (i + 1 == model.transitions.size()) {
      out << "  @";
    }
    out << '\n';
  }
}

void write_place_block(std::ostream& out, const PpnModel& model) {
  out << "place    name    tokens\n";
  for (size_t i = 0; i < model.places.size(); ++i) {
    const auto& place = model.places[i];
    out << i << "        " << place.name << "      " << place.token;
    if (i + 1 == model.places.size()) {
      out << "  @";
    }
    out << '\n';
  }
}

}  // namespace

std::string ppn_to_string(const PpnModel& model) {
  std::ostringstream out;
  write_transition_block(out, model);
  write_place_block(out, model);
  return out.str();
}

bool export_ppn(const PpnModel& model, const std::string& path) {
  std::ofstream out(path);
  if (!out.is_open()) {
    return false;
  }
  out << ppn_to_string(model);
  return static_cast<bool>(out);
}

}  // namespace ptopner_export
