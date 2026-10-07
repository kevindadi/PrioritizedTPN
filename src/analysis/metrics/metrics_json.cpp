#include <fstream>
#include <string>

#include "analysis/metrics/metrics.h"

namespace state_class {

namespace {

std::string time_json(const TimeValue& t) {
  return t.infinite ? std::string("null") : std::to_string(t.value);
}

}  // namespace

bool MetricsAnalyzer::save_to_json(const MetricsReport& report, const std::string& file_path) {
  std::ofstream out(file_path);
  if (!out.is_open()) {
    return false;
  }

  out << "{\n";
  out << "  \"exact\": " << (report.exact ? "true" : "false") << ",\n";
  out << "  \"states\": " << report.states << ",\n";
  out << "  \"transitions\": " << report.transitions << ",\n";
  out << "  \"bounded\": " << (report.bounded ? "true" : "false") << ",\n";
  out << "  \"schedulable\": " << (report.schedulable ? "true" : "false") << ",\n";
  out << "  \"has_steady_cycle\": " << (report.has_steady_cycle ? "true" : "false") << ",\n";
  out << "  \"recurrent_scc_size\": " << report.recurrent_scc_size << ",\n";
  out << "  \"hyperperiod\": " << report.hyperperiod << ",\n";

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
    out << "      \"name\": \"" << t.name << "\",\n";
    out << "      \"core\": " << t.core << ",\n";
    out << "      \"priority\": " << t.priority << ",\n";
    out << "      \"wcet\": " << t.wcet << ",\n";
    out << "      \"bcet\": " << t.bcet << ",\n";
    out << "      \"period\": " << t.period << ",\n";
    out << "      \"deadline\": " << t.deadline << ",\n";
    out << "      \"observed\": " << (t.observed ? "true" : "false") << ",\n";
    out << "      \"activations\": " << t.activations << ",\n";
    out << "      \"wcrt\": " << time_json(t.wcrt) << ",\n";
    out << "      \"bcrt\": " << time_json(t.bcrt) << ",\n";
    out << "      \"jitter\": " << time_json(t.jitter) << ",\n";
    out << "      \"worst_interference\": " << time_json(t.worst_interference) << ",\n";
    out << "      \"worst_blocking\": " << time_json(t.worst_blocking) << ",\n";
    out << "      \"max_preemptions\": " << t.max_preemptions << ",\n";
    out << "      \"max_in_flight\": " << t.max_in_flight << ",\n";
    out << "      \"slack\": " << (t.has_deadline ? time_json(t.slack) : std::string("null"))
        << ",\n";
    out << "      \"jobs_per_hyperperiod\": " << t.jobs_per_hyperperiod << "\n";
    out << "    }" << (i + 1 < report.tasks.size() ? "," : "") << "\n";
  }
  out << "  ],\n";

  out << "  \"locks\": [\n";
  for (size_t i = 0; i < report.locks.size(); ++i) {
    const LockMetrics& l = report.locks[i];
    out << "    {\n";
    out << "      \"name\": \"" << l.name << "\",\n";
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
  return true;
}

}  // namespace state_class
