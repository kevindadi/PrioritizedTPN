#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include "romeo_model.h"
#include "tdg_helpers.h"

namespace romeo {

namespace {

std::string sanitize_identifier(const std::string& raw, const std::string& fallback) {
  const std::string& source = raw.empty() ? fallback : raw;
  std::string result;
  result.reserve(source.size());
  for (char ch : source) {
    const auto uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) || ch == '_') {
      result += ch;
    } else {
      result += '_';
    }
  }
  if (result.empty()) {
    result = fallback;
  }
  if (!std::isalpha(static_cast<unsigned char>(result.front())) && result.front() != '_') {
    result = fallback + "_" + result;
  }
  return result;
}

std::string unique_identifier(const std::string& candidate, std::set<std::string>& used) {
  std::string result = candidate;
  int suffix = 1;
  while (used.count(result) > 0) {
    result = candidate + "_" + std::to_string(suffix++);
  }
  used.insert(result);
  return result;
}

std::string format_int_or_infinity(int value) {
  return value == std::numeric_limits<int>::max() ? "inf" : std::to_string(value);
}

std::string format_interval(const RomeoTimeInterval& interval) {
  return std::string(interval.left_open ? "(" : "[") + format_int_or_infinity(interval.earliest) +
         "," + format_int_or_infinity(interval.latest) +
         std::string(interval.right_open ? ")" : "]");
}

std::string format_assignments(const std::vector<RomeoAssignment>& assignments) {
  if (assignments.empty()) {
    return "";
  }
  std::ostringstream out;
  for (size_t i = 0; i < assignments.size(); ++i) {
    if (i > 0) {
      out << " , ";
    }
    out << assignment_expr(assignments[i].place, assignments[i].delta);
  }
  return out.str();
}

std::string format_transition_options(const RomeoTransition& transition) {
  std::vector<std::string> options;
  if (transition.priority.has_value()) {
    options.push_back("priority=" + std::to_string(*transition.priority));
  }
  if (transition.allow.has_value() && !transition.allow->empty()) {
    options.push_back("allow=" + *transition.allow);
  }
  if (!transition.intermediate.empty()) {
    std::ostringstream intermediate;
    intermediate << "intermediate { " << format_assignments(transition.intermediate) << "; }";
    options.push_back(intermediate.str());
  }
  if (options.empty()) {
    return "";
  }
  std::ostringstream out;
  out << " [";
  for (size_t i = 0; i < options.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << options[i];
  }
  out << "]";
  return out.str();
}

}  // namespace

std::string& RomeoModelBuilder::place(const std::string& name, int initial) {
  for (auto& existing : places_) {
    if (existing.name == name) {
      existing.initial_tokens = initial;
      return existing.name;
    }
  }
  places_.push_back({name, initial});
  return places_.back().name;
}

void RomeoModelBuilder::add_transition(RomeoTransition transition) {
  transitions_.push_back(std::move(transition));
}

std::string render_romeo_cts(const RomeoModel& model) {
  std::set<std::string> used;
  std::vector<std::string> place_ids;
  place_ids.reserve(model.places.size());
  for (size_t i = 0; i < model.places.size(); ++i) {
    place_ids.push_back(unique_identifier(
        sanitize_identifier(model.places[i].name, "P" + std::to_string(i + 1)), used));
  }

  std::map<std::string, std::string> place_map;
  for (size_t i = 0; i < model.places.size(); ++i) {
    place_map[model.places[i].name] = place_ids[i];
  }

  std::ostringstream out;
  out << "// TPN name=PTPN\n\n";
  out << "typedef int place; \n\n";
  out << "initially { \n";
  out << "place ";
  for (size_t i = 0; i < model.places.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << place_ids[i] << "=" << model.places[i].initial_tokens;
  }
  out << "; }\n\n";

  used.clear();
  for (const auto& transition : model.transitions) {
    const std::string transition_id =
        unique_identifier(sanitize_identifier(transition.name, "T"), used);
    out << " transition" << format_transition_options(transition) << "  " << transition_id << " "
        << format_interval(transition.interval) << "\n";
    out << "      when (" << transition.when_guard << ")\n";
    out << "      { ";
    const auto update = format_assignments(transition.updates);
    if (!update.empty()) {
      out << update << "; ";
    }
    out << " }\n";
  }

  out << "\ngraph [passed=eq]\n";
  return out.str();
}

}  // namespace romeo
