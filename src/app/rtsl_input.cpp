#include "app/rtsl_input.h"

#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "common/diagnostics.h"
#include "dsl/lexer.h"
#include "dsl/lower.h"
#include "dsl/parser.h"

namespace app {

std::optional<std::string> read_rtsl_source(const std::string& input_file) {
  std::ifstream input(input_file, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void add_rtsl_open_error(const std::string& input_file, ptpn::Diagnostics& diagnostics) {
  ptpn::SourceLocation location;
  location.file = input_file;
  diagnostics.add("E-SYN-002", "failed to open RTSL file: " + input_file, std::nullopt,
                  std::move(location), "check that the input path exists and is readable");
}

RtslDocument parse_and_validate_rtsl(std::string source, const std::string& input_file,
                                     dsl::ValidationOptions::Mode mode,
                                     ptpn::Diagnostics& diagnostics) {
  RtslDocument document;
  dsl::Lexer lexer(source, input_file, diagnostics);
  std::vector<dsl::Token> tokens = lexer.tokenize();
  dsl::Parser parser(std::move(tokens), input_file, diagnostics, document.source_map);
  document.program = parser.parse();

  dsl::ValidationOptions options;
  options.mode = mode;
  dsl::validate(document.program, document.source_map, diagnostics, options);
  document.valid = diagnostics.ok();
  return document;
}

std::optional<tdg::TDG> lower_rtsl_document(RtslDocument& document,
                                            dsl::ValidationOptions::Mode mode,
                                            ptpn::Diagnostics& diagnostics) {
  if (!document.valid || !diagnostics.ok()) {
    return std::nullopt;
  }
  return dsl::lower(document.program, document.source_map, diagnostics, mode);
}

std::optional<RtslLoadResult> load_tdg_from_rtsl(const std::string& input_file,
                                                 dsl::ValidationOptions::Mode mode,
                                                 ptpn::Diagnostics& diagnostics) {
  std::optional<std::string> source = read_rtsl_source(input_file);
  if (!source.has_value()) {
    add_rtsl_open_error(input_file, diagnostics);
    return std::nullopt;
  }

  RtslDocument document =
      parse_and_validate_rtsl(std::move(source.value()), input_file, mode, diagnostics);
  std::optional<tdg::TDG> lowered = lower_rtsl_document(document, mode, diagnostics);
  if (!lowered.has_value()) {
    return std::nullopt;
  }

  RtslLoadResult result{std::move(lowered.value()), std::move(document.source_map)};
  return result;
}

}  // namespace app
