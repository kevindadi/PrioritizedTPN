#ifndef ANALYSIS_METRICS_METRICS_JSON_H
#define ANALYSIS_METRICS_METRICS_JSON_H

#include <string>

namespace state_class {

struct MetricsReport;

// Writes the performance/schedulability metrics report as JSON. Returns false
// when opening, writing, flushing or closing the output fails.
bool save_metrics_json(const MetricsReport& report, const std::string& file_path);

}  // namespace state_class

#endif  // ANALYSIS_METRICS_METRICS_JSON_H
