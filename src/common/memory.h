#ifndef COMMON_MEMORY_H
#define COMMON_MEMORY_H

#include <cstddef>

namespace ptpn {

// Peak resident set size of this process in megabytes. Linux reports
// ru_maxrss in kilobytes, macOS/Windows in bytes; every branch normalizes to
// MB, so callers must label the value as MB. Returns 0 when the platform
// probe fails.
std::size_t get_memory_usage_mb();

}  // namespace ptpn

#endif  // COMMON_MEMORY_H
