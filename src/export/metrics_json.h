#ifndef EXPORT_METRICS_JSON_H
#define EXPORT_METRICS_JSON_H

#include <string>

namespace state_class {

struct MetricsReport;

// Writes the performance/schedulability metrics report as JSON.
bool save_metrics_json(const MetricsReport& report, const std::string& file_path);

}  // namespace state_class

#endif  // EXPORT_METRICS_JSON_H
