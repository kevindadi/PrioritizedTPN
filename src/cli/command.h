#ifndef CLI_COMMAND_H
#define CLI_COMMAND_H

#include <CLI/CLI.hpp>
#include <cstddef>
#include <memory>
#include <string>

namespace ptpn {
class Diagnostics;
}

namespace cli {

// One CLI subcommand: registers its options on the root application, exposes
// the parsed diagnostics/debug settings to the envelope scaffolding, and runs
// the corresponding CLI-free entry point from the app layer.
class Command {
 public:
  virtual ~Command() = default;

  virtual CLI::App* add_to(CLI::App& root) = 0;

  [[nodiscard]] virtual bool parsed() const = 0;
  [[nodiscard]] virtual bool diagnostics_engaged() const = 0;
  [[nodiscard]] virtual bool diagnostics_stdout() const = 0;
  [[nodiscard]] virtual const std::string& diagnostics_file() const = 0;
  [[nodiscard]] virtual bool debug_mode() const = 0;
  // Commands that print machine-consumable data on stdout (the filled RTSL
  // source) keep every log line on stderr so stdout stays clean.
  [[nodiscard]] virtual bool logs_to_stderr() const { return false; }

  virtual int run(ptpn::Diagnostics* diagnostics, std::size_t initial_memory) = 0;
};

std::unique_ptr<Command> make_tdg_command();
std::unique_ptr<Command> make_rtsl_command();
std::unique_ptr<Command> make_ptpn_command();
std::unique_ptr<Command> make_check_command();
std::unique_ptr<Command> make_describe_command();
std::unique_ptr<Command> make_fill_command();
std::unique_ptr<Command> make_explain_command();
std::unique_ptr<Command> make_allocate_command();

}  // namespace cli

#endif  // CLI_COMMAND_H
