#ifndef APP_LOGGING_H
#define APP_LOGGING_H

#include <string>

namespace app {
namespace detail {

// The `[STATS]` text is parsed by example/run.py; keep the format stable.
void log_step_timing(const std::string& step, long long ms, const std::string& detail = {});

// Temporarily raises the global log level (used to keep per-candidate lowering
// and analysis logging out of the default allocation output). Restores the
// previous level on scope exit, including through early returns.
class ScopedLogLevel {
 public:
  // Accepts an spdlog level value without exposing spdlog in this header.
  explicit ScopedLogLevel(int level);
  ~ScopedLogLevel();

  ScopedLogLevel(const ScopedLogLevel&) = delete;
  ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;

 private:
  int previous_level_;
};

}  // namespace detail
}  // namespace app

#endif  // APP_LOGGING_H
