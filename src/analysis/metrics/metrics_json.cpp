#include <fstream>
#include <string>

#include "analysis/metrics/metrics_json.h"

#include "analysis/metrics/metrics.h"
#include "common/checked_output.h"

namespace state_class {

namespace {

std::string time_json(const TimeValue& t) {
  return t.infinite ? std::string("null") : std::to_string(t.value);
}

// Escapes a string for the hand-written JSON below. Identifiers pass through
// byte-for-byte; only the characters JSON requires are rewritten.
std::string json_escape(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string escaped;
  escaped.reserve(value.size());
  for (unsigned char ch : value) {
    switch (ch) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (ch < 0x20) {
          escaped += "\\u00";
          escaped += kHex[ch >> 4];
          escaped += kHex[ch & 0x0f];
        } else {
          escaped += static_cast<char>(ch);
        }
        break;
    }
  }
  return escaped;
}

}  // namespace

bool save_metrics_json(const MetricsReport& report, const std::string& file_path) {
  std::ofstream out(file_path);
  if (!out.is_open()) {
    return false;
  }

  out << "{\n";
  out << "  \"exact\": " << (report.exact ? "true" : "false") << ",\n";
  out << "  \"wcrt_lb_valid\": " << (report.wcrt_lb_valid ? "true" : "false") << ",\n";
  out << "  \"states\": " << report.states << ",\n";
  out << "  \"transitions\": " << report.transitions << ",\n";
  out << "  \"bounded\": " << (report.bounded ? "true" : "false") << ",\n";
  // Under horizon truncation the graph is incomplete, so schedulability is
  // neither proven nor refuted; null makes that explicit instead of emitting a
  // misleading boolean.
  out << "  \"schedulable\": ";
  if (!report.deadlock_analysis_certified) {
    out << "null";
  } else {
    out << (report.schedulable ? "true" : "false");
  }
  out << ",\n";
  out << "  \"has_steady_cycle\": " << (report.has_steady_cycle ? "true" : "false") << ",\n";
  out << "  \"recurrent_scc_size\": " << report.recurrent_scc_size << ",\n";
  out << "  \"hyperperiod\": " << report.hyperperiod << ",\n";
  // Horizon fields are emitted only when the reduction is active so that
  // exhaustive-output goldens remain stable.
  if (report.horizon.enabled) {
    out << "  \"horizon\": {\n";
    out << "    \"bound\": " << report.horizon.bound << ",\n";
    out << "    \"truncated\": " << (report.horizon.truncated ? "true" : "false") << ",\n";
    out << "    \"pruned_states\": " << report.horizon.pruned_states << ",\n";
    out << "    \"deadlock_analysis_certified\": "
        << (report.deadlock_analysis_certified ? "true" : "false") << "\n";
    out << "  },\n";
  }

  out << "  \"deadlock_states\": [";
  for (size_t i = 0; i < report.deadlock_states.size(); ++i) {
    if (i)
      out << ", ";
    out << report.deadlock_states[i];
  }
  out << "],\n";

  out << "  \"tasks\": [\n";
  for (size_t i = 0; i < report.tasks.size(); ++i) {
    const TaskMetrics& t = report.tasks[i];
    out << "    {\n";
    out << "      \"name\": \"" << json_escape(t.name) << "\",\n";
    out << "      \"core\": " << t.core << ",\n";
    out << "      \"priority\": " << t.priority << ",\n";
    out << "      \"wcet\": " << t.wcet << ",\n";
    out << "      \"bcet\": " << t.bcet << ",\n";
    out << "      \"period\": " << t.period << ",\n";
    out << "      \"deadline\": " << t.deadline << ",\n";
    out << "      \"observed\": " << (t.observed ? "true" : "false") << ",\n";
    out << "      \"activations\": " << t.activations << ",\n";
    out << "      \"wcrt\": " << time_json(t.wcrt) << ",\n";
    out << "      \"wcrt_lb\": "
        << (report.wcrt_lb_valid ? time_json(t.wcrt_lb) : std::string("null")) << ",\n";
    out << "      \"wcrt_interval_tight\": "
        << ((report.wcrt_lb_valid && t.wcrt_interval_tight) ? "true" : "false") << ",\n";
    out << "      \"bcrt\": " << time_json(t.bcrt) << ",\n";
    out << "      \"jitter\": " << time_json(t.jitter) << ",\n";
    out << "      \"worst_interference\": " << time_json(t.worst_interference) << ",\n";
    out << "      \"worst_blocking\": " << time_json(t.worst_blocking) << ",\n";
    out << "      \"max_preemptions\": " << t.max_preemptions << ",\n";
    out << "      \"max_in_flight\": " << t.max_in_flight << ",\n";
    out << "      \"slack\": " << (t.has_deadline ? time_json(t.slack) : std::string("null"))
        << ",\n";
    out << "      \"deadline_missed\": " << (t.deadline_missed ? "true" : "false") << ",\n";
    out << "      \"local_deadlock\": " << (t.local_deadlock ? "true" : "false") << ",\n";
    out << "      \"local_deadlock_scc_size\": " << t.local_deadlock_scc_size << ",\n";
    out << "      \"jobs_per_hyperperiod\": " << t.jobs_per_hyperperiod << "\n";
    out << "    }" << (i + 1 < report.tasks.size() ? "," : "") << "\n";
  }
  out << "  ],\n";

  out << "  \"locks\": [\n";
  for (size_t i = 0; i < report.locks.size(); ++i) {
    const LockMetrics& l = report.locks[i];
    out << "    {\n";
    out << "      \"name\": \"" << json_escape(l.name) << "\",\n";
    out << "      \"worst_hold\": " << time_json(l.worst_hold) << ",\n";
    out << "      \"total_hold\": " << time_json(l.total_hold) << ",\n";
    out << "      \"total_wait\": " << time_json(l.total_wait) << "\n";
    out << "    }" << (i + 1 < report.locks.size() ? "," : "") << "\n";
  }
  out << "  ],\n";

  out << "  \"cores\": [\n";
  for (size_t i = 0; i < report.cores.size(); ++i) {
    const CoreMetrics& c = report.cores[i];
    out << "    {\n";
    out << "      \"core\": " << c.core << ",\n";
    out << "      \"util_min\": " << c.util_min << ",\n";
    out << "      \"util_max\": " << c.util_max << ",\n";
    out << "      \"graph_busy_fraction\": " << c.graph_busy_fraction << "\n";
    out << "    }" << (i + 1 < report.cores.size() ? "," : "") << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return ptpn::finish_output(out);
}

}  // namespace state_class
