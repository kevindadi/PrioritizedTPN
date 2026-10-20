#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace mcp {

// Runs the ptpn CLI in a subprocess. `args` excludes the executable; the
// diagnostics envelope of the child lands in `stdout_text` (the CLI keeps
// stdout reserved for the envelope whenever `--diagnostics json` is passed)
// and its logs in `stderr_text`. Injected so tests can drive the server
// without spawning processes.
using CliRunner =
    std::function<int(const std::vector<std::string>& args, std::string& stdout_text,
                      std::string& stderr_text)>;

// The subprocess runner used by `ptpn mcp`.
CliRunner make_subprocess_runner(std::string executable);

// Path of the running executable, so the MCP tools call the same binary the
// server was started from. Falls back to "ptpn" on PATH when the platform
// probe is unavailable.
std::string default_cli_path();

// Handles one JSON-RPC message and returns the response line (empty for
// notifications). Implements the MCP subset this server needs: initialize,
// notifications/initialized, ping, tools/list and tools/call.
std::string handle_message(const std::string& message, const CliRunner& runner);

// Reads newline-delimited JSON-RPC from `in` and writes one response line per
// request to `out` until the input ends.
void serve(std::istream& in, std::ostream& out, const CliRunner& runner);

}  // namespace mcp

#endif  // MCP_SERVER_H
