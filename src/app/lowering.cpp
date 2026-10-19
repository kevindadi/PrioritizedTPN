#include "app/lowering.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>

#include <spdlog/spdlog.h>

#include "common/diagnostics.h"
#include "common/unsupported_class.h"
#include "dsl/source_map.h"
#include "lower/tdg2pn/tdg2pn.h"
#include "model/petri.h"
#include "model/tdg.h"

namespace app {

namespace {

// The unsupported-class guards in the TDG lowering throw std::invalid_argument
// with a message naming the offending policy/task/lock/edge. Recover a stable
// structured subject for the diagnostic without changing the lowering layer.
std::optional<std::string> unsupported_subject(const std::string& message,
                                               SchedulePolicy policy) {
  const auto quoted_after = [&](const std::string& marker) -> std::optional<std::string> {
    const auto pos = message.find(marker);
    if (pos == std::string::npos) {
      return std::nullopt;
    }
    const auto start = pos + marker.size();
    const auto end = message.find('\'', start);
    if (end == std::string::npos) {
      return std::nullopt;
    }
    return message.substr(start, end - start);
  };

  if (auto task = quoted_after("task '"); task.has_value()) {
    return "task:" + task.value();
  }
  if (auto task = quoted_after("start binding for '"); task.has_value()) {
    return "task:" + task.value();
  }

  const std::string fork_marker = "fork/join nodes: ";
  if (const auto pos = message.find(fork_marker); pos != std::string::npos) {
    return "node:" + message.substr(pos + fork_marker.size());
  }

  const std::string edge_marker = "releases: edge ";
  if (const auto pos = message.find(edge_marker); pos != std::string::npos) {
    std::string edge = message.substr(pos + edge_marker.size());
    const auto stop = edge.find(" connects");
    if (stop != std::string::npos) {
      edge = edge.substr(0, stop);
    }
    std::string compact;
    for (const char c : edge) {
      if (c != ' ') {
        compact += c;
      }
    }
    return "edge:" + compact;
  }

  return "policy:" + schedule_policy_to_string(policy);
}

}  // namespace

// Resolves a lowering-subject key to the RTSL SourceMap entry that anchors it.
// Implicit nodes are bound under `fork:<name>` / `join:<name>` and the policy
// under `policy`, while lowering reports `node:<name>` / `policy:<name>`.
std::optional<ptpn::SourceLocation> source_map_location(const dsl::SourceMap& source_map,
                                                        const std::string& subject) {
  if (const auto location = source_map.location_for(subject); location.has_value()) {
    return location;
  }

  constexpr std::string_view kNodePrefix = "node:";
  if (subject.compare(0, kNodePrefix.size(), kNodePrefix) == 0) {
    const std::string name = subject.substr(kNodePrefix.size());
    if (auto location = source_map.location_for("fork:" + name); location.has_value()) {
      return location;
    }
    if (auto location = source_map.location_for("join:" + name); location.has_value()) {
      return location;
    }
    return std::nullopt;
  }

  constexpr std::string_view kPolicyPrefix = "policy:";
  if (subject.compare(0, kPolicyPrefix.size(), kPolicyPrefix) == 0) {
    return source_map.location_for("policy");
  }
  return std::nullopt;
}

int build_ptpn_from_tdg(const tdg::TDG& tdg, petri::PTPN& ptpn,
                        ptpn::Diagnostics* diagnostics, const dsl::SourceMap* source_map) {
  spdlog::info("\n[PTPN] Converting TDG to PTPN...");
  try {
    converter::TDG2PN::transform(tdg, ptpn);
  } catch (const ptpn::UnsupportedClassError& error) {
    spdlog::error("[PTPN] TDG to PTPN conversion rejected by the supported class: {}",
                  error.what());
    if (diagnostics != nullptr) {
      const std::optional<std::string> subject =
          error.subject().has_value() ? error.subject()
                                      : unsupported_subject(error.what(), tdg.policy);
      std::optional<ptpn::SourceLocation> location;
      if (source_map != nullptr && subject.has_value()) {
        location = source_map_location(*source_map, *subject);
      }
      diagnostics->add("E-UNS-001", error.what(), subject, std::move(location),
                       "simplify the model or choose a supported scheduling policy");
    }
    return 1;
  } catch (const std::exception& error) {
    spdlog::error("[PTPN] TDG to PTPN conversion failed: {}", error.what());
    if (diagnostics != nullptr) {
      diagnostics->add("E-INT-003", error.what(),
                       "policy:" + schedule_policy_to_string(tdg.policy), std::nullopt,
                       "this is an internal lowering failure; report it");
    }
    return 1;
  }
  spdlog::info("[PTPN] PTPN conversion completed");
  spdlog::info("  Places: {}", ptpn.num_places());
  spdlog::info("  Transitions: {}", ptpn.num_transitions());
  spdlog::info("  Policy: {}", schedule_policy_to_string(tdg.policy));
  return 0;
}

}  // namespace app
