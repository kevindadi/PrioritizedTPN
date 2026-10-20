#ifndef APP_DESCRIBE_H
#define APP_DESCRIBE_H

#include <string>

#include "common/diagnostics.h"

namespace dsl::ast {
struct Program;
}

namespace tdg {
class TDG;
}

namespace app {

struct DescribeOptions {
  std::string input_file;
  // Effective equal-priority tie mode (a command-line option, not a model
  // property), reported so the reader sees the semantics the analysis uses.
  std::string tie_mode = "fifo";
  // Optional policy override (`--policy`), applied before the description.
  std::string policy_override;
};

// Restates the model's actual semantics: cores and policy, per-task kind /
// core / priority / period / relative deadline / execution segments with lock
// nesting, release bindings, dependency edges (AND fork/join versus the
// token-consuming alternative/OR semantics of separate statements), lock
// usage, and the validation hints that flag the common traps. `program` is the
// RTSL AST when available (it carries the edge-statement structure and the
// pending-measurement state); `validation` supplies the notes section.
std::string build_model_description(const tdg::TDG& tdg, const dsl::ast::Program* program,
                                    const ptpn::Diagnostics& validation,
                                    const DescribeOptions& options);

// `ptpn describe -f model.rtsl`: parse, validate, lower, and print the
// description. Returns the exit code to use when the diagnostics transport is
// not engaged.
int run_rtsl_describe(const DescribeOptions& options, ptpn::Diagnostics* diagnostics);

}  // namespace app

#endif  // APP_DESCRIBE_H
