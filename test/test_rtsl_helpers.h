#ifndef TEST_RTSL_HELPERS_H
#define TEST_RTSL_HELPERS_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "common/diagnostics.h"
#include "dsl/ast.h"
#include "dsl/lexer.h"
#include "dsl/parser.h"
#include "dsl/source_map.h"
#include "dsl/validate.h"

namespace rtsl_test {

inline std::vector<dsl::Token> lex_source(const std::string& source, ptpn::Diagnostics& diagnostics,
                                          const std::string& file = "test.rtsl") {
  dsl::Lexer lexer(source, file, diagnostics);
  return lexer.tokenize();
}

inline dsl::ast::Program parse_source(const std::string& source, ptpn::Diagnostics& diagnostics,
                                      dsl::SourceMap& source_map,
                                      const std::string& file = "test.rtsl") {
  std::vector<dsl::Token> tokens = lex_source(source, diagnostics, file);
  dsl::Parser parser(std::move(tokens), file, diagnostics, source_map);
  return parser.parse();
}

inline dsl::ast::Program check_source(const std::string& source, ptpn::Diagnostics& diagnostics,
                                      dsl::SourceMap& source_map,
                                      const dsl::ValidationOptions& options = {},
                                      const std::string& file = "test.rtsl") {
  dsl::ast::Program program = parse_source(source, diagnostics, source_map, file);
  dsl::validate(program, source_map, diagnostics, options);
  return program;
}

inline std::string system_block(const std::string& body,
                                const std::string& attributes = "  cpus 4\n") {
  return "system demo {\n" + attributes + body + "\n}\n";
}

inline const ptpn::Diagnostic* find_diagnostic(const ptpn::Diagnostics& diagnostics,
                                               const std::string& code) {
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    if (diagnostic.code == code) {
      return &diagnostic;
    }
  }
  return nullptr;
}

inline std::vector<const ptpn::Diagnostic*> diagnostics_with_code(
    const ptpn::Diagnostics& diagnostics, const std::string& code) {
  std::vector<const ptpn::Diagnostic*> matches;
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    if (diagnostic.code == code) {
      matches.push_back(&diagnostic);
    }
  }
  return matches;
}

inline std::size_t count_code(const ptpn::Diagnostics& diagnostics, const std::string& code) {
  std::size_t count = 0;
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

}  // namespace rtsl_test

#endif  // TEST_RTSL_HELPERS_H
