#ifndef PTPN_TEST_TDG_TEST_HELPERS_H
#define PTPN_TEST_TDG_TEST_HELPERS_H

#include <stdexcept>
#include <string>

#include "model/tdg.h"
#include "parse/json.h"

namespace ptpn_test {

// Parses and validates `path` once, then populates the TDG from that parse.
inline tdg::TDG load_tdg(const std::string& path) {
  parse::Parser parser;
  const auto result = parser.parse_file(path);
  if (!result.success) {
    throw std::runtime_error("failed to parse " + path + ": " + result.error_message);
  }
  tdg::TDG tdg(parser.get_num_cpus(), parser.get_cores_per_cpu());
  tdg.load_from_parser(parser);
  return tdg;
}

}  // namespace ptpn_test

#endif  // PTPN_TEST_TDG_TEST_HELPERS_H
