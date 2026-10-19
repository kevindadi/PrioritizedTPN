#include "app/check_cmd.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "common/diagnostics.h"
#include "dsl/lexer.h"
#include "dsl/parser.h"
#include "dsl/source_map.h"
#include "dsl/validate.h"

namespace app {

namespace {

void print_legacy_diagnostics(const ptpn::Diagnostics& diagnostics) {
  for (const ptpn::Diagnostic& diagnostic : diagnostics.entries()) {
    std::cerr << "ERROR: ";
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

}  // namespace

int run_rtsl_check(const CheckCommandOptions& opts, ptpn::Diagnostics* diagnostics) {
  ptpn::Diagnostics local;
  ptpn::Diagnostics& collector = diagnostics != nullptr ? *diagnostics : local;

  std::ifstream input(opts.input_file, std::ios::binary);
  if (!input) {
    const std::string message = "failed to open RTSL file: " + opts.input_file;
    if (diagnostics != nullptr) {
      ptpn::SourceLocation location;
      location.file = opts.input_file;
      diagnostics->add("E-SYN-002", message, std::nullopt, location,
                       "check that the input path exists and is readable");
      return diagnostics->exit_code();
    }
    std::cerr << "ERROR: " << message << '\n';
    return 1;
  }

  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());

  dsl::SourceMap source_map;
  dsl::Lexer lexer(source, opts.input_file, collector);
  std::vector<dsl::Token> tokens = lexer.tokenize();
  dsl::Parser parser(std::move(tokens), opts.input_file, collector, source_map);
  const dsl::ast::Program program = parser.parse();
  dsl::validate(program, source_map, collector, dsl::ValidationOptions{});

  const bool ok = collector.ok();
  if (ok) {
    collector.set_result(nlohmann::json{
        {"pending_measurements", dsl::collect_pending_measurements(program, source_map)}});
  }

  if (diagnostics == nullptr) {
    print_legacy_diagnostics(collector);
  }

  std::cerr << "RTSL check: " << program.tasks.size() << " tasks, " << program.edges.size()
            << " edges, " << (ok ? std::string("ok") : std::string("failed")) << '\n';

  if (diagnostics == nullptr) {
    return ok ? 0 : 1;
  }
  return collector.exit_code();
}

}  // namespace app
