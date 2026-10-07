#ifndef EXPORT_SCG_DOT_H
#define EXPORT_SCG_DOT_H

#include <cstddef>
#include <string>
#include <vector>

#include "analysis/ptpn_analysis.h"
#include "analysis/state_class.h"
#include "model/petri.h"

namespace scg_export {

// Renders a state-class reachability graph (plus the net that owns it) to
// Graphviz DOT and to the human-readable text/HTML labels used by --export-scg.
class ScgFormatter {
 public:
  ScgFormatter(const petri::PTPN& net, const state_class::SCGraph& graph);

  [[nodiscard]] std::string format_transition_label(size_t transition_id) const;
  [[nodiscard]] std::string format_transitions(const state_class::TransitionSet& transitions) const;
  [[nodiscard]] std::string format_named_dbm(const state_class::StateClass& state) const;
  [[nodiscard]] std::string format_state_dump(const state_class::StateClass& state) const;
  [[nodiscard]] std::string format_state_label_html(const state_class::StateClass& state) const;
  [[nodiscard]] std::string render_dot() const;
  bool save_to_dot(const std::string& file_path) const;

 private:
  static std::string format_marking(const petri::PTPN& net, const std::vector<int>& marking);
  [[nodiscard]] std::vector<std::string> format_zone_constraints(
      const state_class::StateClass& state, bool html) const;

  const petri::PTPN& net_;
  const state_class::SCGraph& graph_;
};

}  // namespace scg_export

#endif  // EXPORT_SCG_DOT_H
