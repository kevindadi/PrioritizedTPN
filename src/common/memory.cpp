#if defined(_WIN32)
#  include <psapi.h>
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach/mach_init.h>
#  include <mach/task.h>
#else
#  include <sys/resource.h>
#endif

#include "common/memory.h"

namespace ptpn {

std::size_t get_memory_usage_mb() {
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
    return static_cast<std::size_t>(usage.ru_maxrss) / 1024;
  }
  return 0;
#endif
}

}  // namespace ptpn
