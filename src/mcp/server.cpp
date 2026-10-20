#include "mcp/server.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(__linux__)
#  include <unistd.h>
#endif

namespace mcp {

namespace {

using nlohmann::json;

constexpr const char* kProtocolVersion = "2024-11-05";

std::string shell_quote(const std::string& value) {
#if defined(_WIN32)
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += "\"";
#else
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
#endif
  return quoted;
}

std::string trim(const std::string& value) {
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The analysis command for an input path: `.rtsl` lowers in process, `.json`
// is a TDG, `.ptpn` is a Petri net source.
std::string analysis_command_for(const std::string& file) {
  if (ends_with(file, ".rtsl")) {
    return "rtsl";
  }
  if (ends_with(file, ".ptpn")) {
    return "ptpn";
  }
  return "tdg";
}

// Appends a numeric option when the argument is present.
void add_number(const json& arguments, const char* name, const char* flag,
                std::vector<std::string>& args) {
  const auto it = arguments.find(name);
  if (it == arguments.end() || it->is_null()) {
    return;
  }
  if (it->is_number_integer()) {
    args.push_back(flag);
    args.push_back(std::to_string(it->get<long long>()));
  } else if (it->is_number_float()) {
    args.push_back(flag);
    args.push_back(std::to_string(it->get<double>()));
  } else if (it->is_string()) {
    args.push_back(flag);
    args.push_back(it->get<std::string>());
  }
}

void add_flag(const json& arguments, const char* name, const char* flag,
              std::vector<std::string>& args) {
  const auto it = arguments.find(name);
  if (it != arguments.end() && it->is_boolean() && it->get<bool>()) {
    args.push_back(flag);
  }
}

// Writes the inline `values` object to a temporary file for `--values`.
std::string write_values_file(const json& values, std::string& error) {
  const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                           "/ptpn-mcp-values-" + std::to_string(std::rand()) + ".json";
  std::ofstream stream(path, std::ios::trunc);
  if (!stream || !(stream << values.dump(2))) {
    error = "cannot write the temporary values file " + path;
    return {};
  }
  return path;
}

struct ToolResult {
  bool ok = true;
  std::string text;
  json structured;
};

ToolResult missing_argument(const std::string& name) {
  ToolResult result;
  result.ok = false;
  result.text = "missing required argument '" + name + "'";
  return result;
}

// Every tool shells out to the same binary with `--diagnostics json`; the
// envelope becomes both the text block and the structured content.
ToolResult run_envelope(const CliRunner& runner, const std::vector<std::string>& args,
                        const std::string& values_file) {
  std::vector<std::string> full = args;
  full.push_back("--diagnostics");
  full.push_back("json");

  std::string stdout_text;
  std::string stderr_text;
  const int exit_code = runner(full, stdout_text, stderr_text);
  if (!values_file.empty()) {
    std::remove(values_file.c_str());
  }

  ToolResult result;
  const std::string envelope_text = trim(stdout_text);
  const json envelope = json::parse(envelope_text, nullptr, false);
  if (!envelope.is_discarded()) {
    result.structured = envelope;
    result.text = envelope.dump(2);
  } else {
    // The child did not produce an envelope (a crash or a usage error before
    // the transport was set up): report the raw streams instead.
    result.text = "exit code " + std::to_string(exit_code) + "\nstdout:\n" + stdout_text +
                  "\nstderr:\n" + trim(stderr_text);
    result.structured = json{{"exit_code", exit_code},
                             {"stdout", trim(stdout_text)},
                             {"stderr", trim(stderr_text)}};
  }
  result.ok = exit_code != 1;
  return result;
}

json object_schema(json properties, std::vector<std::string> required) {
  json schema = {{"type", "object"}, {"properties", std::move(properties)}};
  if (!required.empty()) {
    schema["required"] = std::move(required);
  }
  return schema;
}

json tool_definitions() {
  const json file_property = {{"type", "string"},
                              {"description", "Path to the model (.rtsl, .json TDG or .ptpn)"}};
  const json analysis_properties = {
      {"file", file_property},
      {"max_states", {{"type", "integer"}, {"description", "State-class bound (default 20000)"}}},
      {"horizon", {{"type", "integer"}, {"description", "Time horizon in engine ticks (ms)"}}},
      {"timeout", {{"type", "number"}, {"description", "Wall-clock budget in seconds"}}},
      {"max_memory", {{"type", "integer"}, {"description", "Process resident-set ceiling in MB"}}},
      {"no_exact_wcrt",
       {{"type", "boolean"}, {"description", "Disable the exact WCRT observation"}}},
      {"tie", {{"type", "string"}, {"enum", {"fifo", "arbitrary"}}}},
      {"policy", {{"type", "string"}, {"description", "Override the scheduling policy"}}},
      {"command", {{"type", "string"},
                   {"enum", {"rtsl", "tdg", "ptpn"}},
                   {"description", "Override the input front-end (default: by extension)"}}}};

  const json fill_properties = {
      {"file", file_property},
      {"values", {{"type", "object"}, {"description", "task -> [[min, max], ...]"}}},
      {"task", {{"type", "string"}}},
      {"intervals", {{"type", "string"}, {"description", "e.g. \"[1,2],[3,4],[1,2]\""}}},
      {"in_place", {{"type", "boolean"}}},
      {"overwrite", {{"type", "boolean"}}}};

  json tools = json::array();
  tools.push_back({{"name", "ptpn_check"},
                   {"description",
                    "Lex, parse and validate an RTSL model. Returns the diagnostics envelope; "
                    "pending `measured by llm` tasks appear in result.pending_measurements."},
                   {"inputSchema", object_schema({{"file", file_property}}, {"file"})}});
  tools.push_back({{"name", "ptpn_analyze"},
                   {"description",
                    "Analyze a model and return the diagnostics envelope with WCRT, verdicts and "
                    "schedulability (result.analysis). Exit codes: 0 ok, 3 validation, 4 "
                    "unsupported, 5 partial (budget or truncation), 6 counterexample."},
                   {"inputSchema", object_schema(analysis_properties, {"file"})}});
  tools.push_back({{"name", "ptpn_describe"},
                   {"description",
                    "Restate the model's effective semantics (policy, computed priorities, core "
                    "mapping, lock nesting, release phases, AND fork/join versus alternative/OR "
                    "edges) as result.description."},
                   {"inputSchema", object_schema({{"file", file_property}}, {"file"})}});
  tools.push_back(
      {{"name", "ptpn_fill"},
       {"description",
        "Write measured intervals into pending `measured by llm` declarations. Provide either "
        "`values` (task -> [[min, max], ...]) or `task` with `intervals`. Values are engine ticks "
        "(milliseconds). The patched source is returned as result.source unless in_place is true."},
       {"inputSchema", object_schema(fill_properties, {"file"})}});
  tools.push_back(
      {{"name", "ptpn_explain"},
       {"description",
        "Explain one diagnostic code (or list the whole catalog when `code` is omitted). The "
        "catalog is generated from docs/diagnostics.md."},
       {"inputSchema", object_schema({{"code", {{"type", "string"}}}}, {})}});
  return tools;
}

ToolResult call_tool(const std::string& name, const json& arguments, const CliRunner& runner) {
  if (!arguments.is_object()) {
    ToolResult result;
    result.ok = false;
    result.text = "arguments must be an object";
    return result;
  }
  const auto file_argument = [&](std::string& file) {
    const auto it = arguments.find("file");
    if (it == arguments.end() || !it->is_string() || it->get<std::string>().empty()) {
      return false;
    }
    file = it->get<std::string>();
    return true;
  };

  if (name == "ptpn_check") {
    std::string file;
    if (!file_argument(file)) {
      return missing_argument("file");
    }
    return run_envelope(runner, {"check", "-f", file}, {});
  }
  if (name == "ptpn_describe") {
    std::string file;
    if (!file_argument(file)) {
      return missing_argument("file");
    }
    return run_envelope(runner, {"describe", "-f", file}, {});
  }
  if (name == "ptpn_analyze") {
    std::string file;
    if (!file_argument(file)) {
      return missing_argument("file");
    }
    std::vector<std::string> args;
    const auto command_it = arguments.find("command");
    args.push_back(command_it != arguments.end() && command_it->is_string()
                       ? command_it->get<std::string>()
                       : analysis_command_for(file));
    args.push_back("-f");
    args.push_back(file);
    add_number(arguments, "max_states", "-m", args);
    add_number(arguments, "horizon", "--horizon", args);
    add_number(arguments, "timeout", "--timeout", args);
    add_number(arguments, "max_memory", "--max-memory", args);
    add_number(arguments, "policy", "--policy", args);
    add_number(arguments, "tie", "--tie", args);
    add_flag(arguments, "no_exact_wcrt", "--no-exact-wcrt", args);
    return run_envelope(runner, args, {});
  }
  if (name == "ptpn_fill") {
    std::string file;
    if (!file_argument(file)) {
      return missing_argument("file");
    }
    std::vector<std::string> args{"fill", "-f", file};
    std::string values_file;
    const auto values_it = arguments.find("values");
    const auto task_it = arguments.find("task");
    if (values_it != arguments.end() && values_it->is_object()) {
      std::string error;
      values_file = write_values_file(*values_it, error);
      if (values_file.empty()) {
        ToolResult result;
        result.ok = false;
        result.text = error;
        return result;
      }
      args.push_back("--values");
      args.push_back(values_file);
    } else if (task_it != arguments.end() && task_it->is_string()) {
      const auto intervals_it = arguments.find("intervals");
      if (intervals_it == arguments.end() || !intervals_it->is_string()) {
        return missing_argument("intervals");
      }
      args.push_back("--task");
      args.push_back(task_it->get<std::string>());
      args.push_back("--intervals");
      args.push_back(intervals_it->get<std::string>());
    } else {
      return missing_argument("values or task");
    }
    add_flag(arguments, "in_place", "--in-place", args);
    add_flag(arguments, "overwrite", "--overwrite", args);
    return run_envelope(runner, args, values_file);
  }
  if (name == "ptpn_explain") {
    std::vector<std::string> args{"explain"};
    const auto code_it = arguments.find("code");
    if (code_it != arguments.end() && code_it->is_string() && !code_it->get<std::string>().empty()) {
      args.push_back(code_it->get<std::string>());
    }
    return run_envelope(runner, args, {});
  }

  ToolResult result;
  result.ok = false;
  result.text = "unknown tool '" + name + "'";
  return result;
}

json success_response(const json& id, json result) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json error_response(const json& id, int code, const std::string& message) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json tool_result_content(const ToolResult& result) {
  json response = {{"content", json::array({{{"type", "text"}, {"text", result.text}}})},
                   {"isError", !result.ok}};
  if (result.structured.is_object()) {
    response["structuredContent"] = result.structured;
  }
  return response;
}

json dispatch(const json& request, const CliRunner& runner) {
  const json id = request.contains("id") ? request.at("id") : json();
  const auto method_it = request.find("method");
  if (method_it == request.end() || !method_it->is_string()) {
    return error_response(id, -32600, "invalid request: 'method' must be a string");
  }
  const std::string method = method_it->get<std::string>();
  const json params = request.contains("params") && request.at("params").is_object()
                          ? request.at("params")
                          : json::object();

  if (method == "initialize") {
    return success_response(id,
                            {{"protocolVersion", kProtocolVersion},
                             {"capabilities", {{"tools", json::object()}}},
                             {"serverInfo", {{"name", "ptpn"}, {"version", "1.0.0"}}}});
  }
  if (method == "notifications/initialized" || method == "notifications/cancelled") {
    return json();  // notification: no response
  }
  if (method == "ping") {
    return success_response(id, json::object());
  }
  if (method == "tools/list") {
    return success_response(id, {{"tools", tool_definitions()}});
  }
  if (method == "tools/call") {
    const auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()) {
      return error_response(id, -32602, "invalid params: 'name' is required");
    }
    const json arguments = params.contains("arguments") && params.at("arguments").is_object()
                               ? params.at("arguments")
                               : json::object();
    return success_response(id, tool_result_content(call_tool(name_it->get<std::string>(),
                                                              arguments, runner)));
  }
  return error_response(id, -32601, "method not found: " + method);
}

}  // namespace

CliRunner make_subprocess_runner(std::string executable) {
  return [executable = std::move(executable)](const std::vector<std::string>& args,
                                              std::string& stdout_text,
                                              std::string& stderr_text) {
    const std::string stderr_path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR")
                                                                     : "/tmp") +
                                    "/ptpn-mcp-stderr-" + std::to_string(std::rand()) + ".log";
    std::string command = shell_quote(executable);
    for (const std::string& argument : args) {
      command += " " + shell_quote(argument);
    }
    command += " 2>" + shell_quote(stderr_path);

    stdout_text.clear();
    stderr_text.clear();
    int exit_code = 1;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
      stderr_text = "cannot spawn " + executable;
      return exit_code;
    }
    std::array<char, 4096> buffer{};
    std::size_t read_bytes = 0;
    while ((read_bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
      stdout_text.append(buffer.data(), read_bytes);
    }
    const int status = pclose(pipe);
#if defined(WIFEXITED) && defined(WEXITSTATUS)
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#else
    exit_code = status;
#endif
    std::ifstream error_stream(stderr_path);
    if (error_stream) {
      std::ostringstream contents;
      contents << error_stream.rdbuf();
      stderr_text = contents.str();
    }
    std::remove(stderr_path.c_str());
    return exit_code;
  };
}

std::string default_cli_path() {
#if defined(__linux__)
  std::array<char, 4096> buffer{};
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length > 0) {
    return std::string(buffer.data(), static_cast<std::size_t>(length));
  }
#endif
  return "ptpn";
}

std::string handle_message(const std::string& message, const CliRunner& runner) {
  const std::string line = trim(message);
  if (line.empty()) {
    return {};
  }
  const json request = json::parse(line, nullptr, false);
  if (request.is_discarded()) {
    return error_response(json(), -32700, "parse error").dump();
  }
  if (!request.is_object()) {
    return error_response(json(), -32600, "invalid request: expected an object").dump();
  }
  const bool notification = !request.contains("id");
  json response = dispatch(request, runner);
  if (notification || response.is_null()) {
    return {};
  }
  return response.dump();
}

void serve(std::istream& in, std::ostream& out, const CliRunner& runner) {
  std::string line;
  while (std::getline(in, line)) {
    const std::string response = handle_message(line, runner);
    if (response.empty()) {
      continue;
    }
    out << response << "\n";
    out.flush();
  }
}

}  // namespace mcp
