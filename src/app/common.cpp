#if defined(_WIN32)
#  include <psapi.h>
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach/mach_init.h>
#  include <mach/task.h>
#else
#  include <sys/resource.h>
#  include <unistd.h>
#endif

#include "app/common.h"

#include <iostream>

#include "common/diagnostics.h"

namespace app {

size_t get_memory_usage() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return pmc.WorkingSetSize / 1024 / 1024;
  }
  return 0;
#elif defined(__APPLE__)
  const task_t task = mach_task_self();
  struct task_basic_info t_info{};
  mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;
  if (task_info(task, TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&t_info), &t_info_count) ==
      KERN_SUCCESS) {
    return t_info.resident_size / 1024 / 1024;
  }
  return 0;
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    return usage.ru_maxrss / 1024;
  }
  return 0;
#endif
}

void print_legacy_diagnostics(const ptpn::Diagnostics& diagnostics) {
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    if (diagnostic.severity == "hint") {
      std::cerr << "HINT: ";
    } else if (diagnostic.severity == "warning") {
      std::cerr << "WARNING: ";
    } else {
      std::cerr << "ERROR: ";
    }
    if (diagnostic.location.has_value()) {
      const ptpn::SourceLocation& location = diagnostic.location.value();
      if (!location.file.empty()) {
        std::cerr << location.file;
        if (location.line > 0) {
          std::cerr << ':' << location.line;
          if (location.column > 0) {
            std::cerr << ':' << location.column;
          }
        }
        std::cerr << ": ";
      }
    }
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (diagnostic.fix.has_value() && !diagnostic.fix->empty()) {
      std::cerr << "  fix: " << diagnostic.fix.value() << '\n';
    }
  }
  if (diagnostics.truncated()) {
    std::cerr << "ERROR: additional diagnostics were truncated (cap "
              << ptpn::Diagnostics::kMaxDiagnostics << ")\n";
  }
}

}  // namespace app
