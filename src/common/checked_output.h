#ifndef COMMON_CHECKED_OUTPUT_H
#define COMMON_CHECKED_OUTPUT_H

#include <fstream>

namespace ptpn {
// Flushes buffered data and closes the stream. Returns false when any part of
// the write pipeline failed, including errors that only surface at flush or
// close. Call after the last insertion and before logging success.
inline bool finish_output(std::ofstream& out) {
  out.flush();
  if (!out) {
    return false;
  }
  out.close();
  return static_cast<bool>(out);
}
}  // namespace ptpn

#endif  // COMMON_CHECKED_OUTPUT_H
