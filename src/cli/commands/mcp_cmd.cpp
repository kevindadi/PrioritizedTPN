#include "mcp/server.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

#include "cli/command.h"
#include "cli/option_groups.h"

namespace cli {
namespace {

class McpCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand(
        "mcp", "Serve the analyzer over MCP (JSON-RPC on stdin/stdout) for LLM clients");
    add_diagnostics_options(app_, diagnostics_);
    app_->footer(
        "Reads newline-delimited JSON-RPC 2.0 messages on stdin and writes one\n"
        "response per request to stdout. Implements initialize, ping,\n"
        "tools/list and tools/call for ptpn_check, ptpn_analyze, ptpn_describe,\n"
        "ptpn_fill and ptpn_explain; each tool shells out to this same binary\n"
        "with --diagnostics json and returns that envelope as the tool result.\n"
        "\n"
        "Example (an MCP client configuration):\n"
        "  command: /path/to/ptpn\n"
        "  args:    [mcp]");
    return app_;
  }

  [[nodiscard]] bool parsed() const override {
    return app_ != nullptr && app_->parsed();
  }

  [[nodiscard]] bool diagnostics_engaged() const override {
    return diagnostics_.engaged();
  }

  [[nodiscard]] bool diagnostics_stdout() const override {
    return diagnostics_.output == "json";
  }

  [[nodiscard]] const std::string& diagnostics_file() const override {
    return diagnostics_.file;
  }

  [[nodiscard]] bool debug_mode() const override {
    return false;
  }

  // stdout carries the protocol, so every log line belongs on stderr.
  [[nodiscard]] bool logs_to_stderr() const override {
    return true;
  }

  int run(ptpn::Diagnostics* /*diagnostics*/, std::size_t /*initial_memory*/) override {
    const mcp::CliRunner runner = mcp::make_subprocess_runner(mcp::default_cli_path());
    mcp::serve(std::cin, std::cout, runner);
    return 0;
  }

 private:
  CLI::App* app_ = nullptr;
  app::DiagnosticsOptions diagnostics_;
};

}  // namespace

std::unique_ptr<Command> make_mcp_command() {
  return std::make_unique<McpCommand>();
}

}  // namespace cli
