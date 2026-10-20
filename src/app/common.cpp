#include "app/common.h"

#include <iostream>

#include "common/diagnostics.h"

namespace app {

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
