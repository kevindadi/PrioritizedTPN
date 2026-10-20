#include "app/logging.h"

#include <spdlog/spdlog.h>

namespace app {
namespace detail {

void log_step_timing(const std::string& step, long long ms, const std::string& detail) {
  if (detail.empty()) {
    spdlog::info("[STATS] {}: {} ms", step, ms);
  } else {
    spdlog::info("[STATS] {}: {} ms ({})", step, ms, detail);
  }
}

ScopedLogLevel::ScopedLogLevel(int level)
    : previous_level_(static_cast<int>(spdlog::default_logger()->level())) {
  spdlog::set_level(static_cast<spdlog::level::level_enum>(level));
}

ScopedLogLevel::~ScopedLogLevel() {
  spdlog::set_level(static_cast<spdlog::level::level_enum>(previous_level_));
}

}  // namespace detail
}  // namespace app
