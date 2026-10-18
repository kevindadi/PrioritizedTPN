#include "tdg2romeo.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

#include "inhibitor_arc.h"
#include "scheduling_net.h"

namespace romeo {

RomeoModel build_romeo_model(const tdg::TDG& tdg, const RomeoExportOptions& opts) {
  switch (opts.format) {
    case RomeoFormat::InhibitorArc:
      return build_inhibitor_arc_model(tdg, opts);
    case RomeoFormat::SchedulingNet:
    default:
      return build_scheduling_net_model(tdg, opts);
  }
}

RomeoExportResult export_tdg_to_romeo_cts(const tdg::TDG& tdg, const std::string& output_path,
                                          const RomeoExportOptions& opts) {
  RomeoExportResult result;
  try {
    const RomeoModel model = build_romeo_model(tdg, opts);
    const std::string cts_text = render_romeo_cts(model);

    boost::filesystem::path cts_path(output_path);
    if (!cts_path.parent_path().empty() && !boost::filesystem::exists(cts_path.parent_path())) {
      boost::filesystem::create_directories(cts_path.parent_path());
    }

    std::ofstream out(cts_path.string());
    if (!out) {
      result.error_message = "Cannot open Romeo CTS file: " + output_path;
      return result;
    }
    out << cts_text;
    out.close();

    spdlog::info("[TDG2ROMEO] Exported {} places, {} transitions to {} (format={})",
                 model.places.size(), model.transitions.size(), output_path,
                 opts.format == RomeoFormat::InhibitorArc ? "inhibitor-arc" : "scheduling-net");
    result.success = true;
  } catch (const std::exception& e) {
    result.error_message = e.what();
    spdlog::error("[TDG2ROMEO] Export failed: {}", e.what());
  }
  return result;
}

}  // namespace romeo
