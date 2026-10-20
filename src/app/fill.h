#ifndef APP_FILL_H
#define APP_FILL_H

#include <string>

#include "common/diagnostics.h"

namespace app {

struct FillOptions {
  std::string input_file;
  // JSON object mapping task names to interval lists:
  //   {"fusion": [[1, 2], [3, 4], [1, 2]], "sensor": [[2, 3]]}
  // Values are engine ticks (milliseconds); `fill` writes them with an explicit
  // `ms` suffix when the system's time unit is seconds so the meaning never
  // depends on `time_unit`.
  std::string values_file;
  // Single-task alternative to --values: --task NAME --intervals "[1,2],[3,4]".
  std::string task;
  std::string intervals;
  // Destination: `--output PATH`, `--in-place`, or stdout (the envelope's
  // result.source when the diagnostics transport is engaged).
  std::string output_file;
  bool in_place = false;
  // Also replace tasks whose `time` already has intervals (re-measurement).
  bool overwrite = false;
};

// Replaces pending `time measured by llm { ... }` declarations with the
// measured intervals, keeping the original provenance clause (runs, timeout,
// artifact) verbatim. The patched source is re-parsed and re-validated before
// it is written, so `fill` never produces an invalid model. Returns the exit
// code to use when the diagnostics transport is not engaged.
int run_rtsl_fill(const FillOptions& options, ptpn::Diagnostics* diagnostics);

}  // namespace app

#endif  // APP_FILL_H
