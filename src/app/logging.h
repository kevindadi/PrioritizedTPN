#ifndef APP_LOGGING_H
#define APP_LOGGING_H

#include <spdlog/spdlog.h>
#include <string>

namespace app {
namespace detail {

// The `[STATS]` text is parsed by example/run.py; keep the format stable.
inline void log_step_timing(const std::string& step, long long ms,
                            const std::string& detail = {}) {
  if (detail.empty()) {
    spdlog::info("[STATS] {}: {} ms", step, ms);
  } else {
    spdlog::info("[STATS] {}: {} ms ({})", step, ms, detail);
  }
}

// Temporarily raises the global log level (used to keep per-candidate lowering
// and analysis logging out of the default allocation output). Restores the
// previous level on scope exit, including through early returns.
class ScopedLogLevel {
 public:
  explicit ScopedLogLevel(spdlog::level::level_enum level)
      : previous_level_(spdlog::default_logger()->level()) {
    spdlog::set_level(level);
  }

  ~ScopedLogLevel() {
    spdlog::set_level(previous_level_);
  }

  ScopedLogLevel(const ScopedLogLevel&) = delete;
  ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;

 private:
  spdlog::level::level_enum previous_level_;
};

}  // namespace detail
}  // namespace app

#endif  // APP_LOGGING_H
