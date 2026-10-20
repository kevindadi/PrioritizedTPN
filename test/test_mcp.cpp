#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cli_test_helpers.h"
#include "mcp/server.h"

#if defined(__unix__) || defined(__APPLE__)
#  include <cstdio>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

using nlohmann::json;

// Records every CLI invocation and answers with a canned envelope.
struct FakeCli {
  std::vector<std::vector<std::string>> calls;
  int exit_code = 0;
  std::string stdout_text = R"({"exit_code":0,"ok":true,"protocol":"ptpn-diagnostics","result":{}})";

  mcp::CliRunner runner() {
    return [this](const std::vector<std::string>& args, std::string& out, std::string& err) {
      calls.push_back(args);
      out = stdout_text;
      err.clear();
      return exit_code;
    };
  }
};

json request(const std::string& method, const json& params = json::object(), int id = 1) {
  json message = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
  if (!params.empty()) {
    message["params"] = params;
  }
  return message;
}

json response_of(const std::string& line) {
  const json parsed = json::parse(line, nullptr, false);
  EXPECT_FALSE(parsed.is_discarded()) << line;
  return parsed;
}

}  // namespace

TEST(McpTest, InitializeAndPing) {
  FakeCli cli;
  const json init = response_of(mcp::handle_message(request("initialize").dump(), cli.runner()));
  EXPECT_EQ("2024-11-05", init.at("result").at("protocolVersion").get<std::string>());
  EXPECT_EQ("ptpn", init.at("result").at("serverInfo").at("name").get<std::string>());
  EXPECT_TRUE(init.at("result").at("capabilities").contains("tools"));

  const json ping = response_of(mcp::handle_message(request("ping", json::object(), 7).dump(),
                                                    cli.runner()));
  EXPECT_EQ(7, ping.at("id").get<int>());
  EXPECT_TRUE(ping.at("result").is_object());
}

TEST(McpTest, NotificationsProduceNoResponse) {
  FakeCli cli;
  EXPECT_TRUE(mcp::handle_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                                  cli.runner())
                  .empty());
  EXPECT_TRUE(mcp::handle_message(R"({"jsonrpc":"2.0","method":"notifications/cancelled"})",
                                  cli.runner())
                  .empty());
}

TEST(McpTest, ToolsListDescribesEveryTool) {
  FakeCli cli;
  const json listed = response_of(mcp::handle_message(request("tools/list").dump(), cli.runner()));
  const json& tools = listed.at("result").at("tools");
  ASSERT_EQ(5u, tools.size());
  std::vector<std::string> names;
  for (const json& tool : tools) {
    names.push_back(tool.at("name").get<std::string>());
    EXPECT_FALSE(tool.at("description").get<std::string>().empty());
    EXPECT_EQ("object", tool.at("inputSchema").at("type").get<std::string>());
  }
  EXPECT_EQ((std::vector<std::string>{"ptpn_check", "ptpn_analyze", "ptpn_describe", "ptpn_fill",
                                      "ptpn_explain"}),
            names);
}

TEST(McpTest, CheckAndExplainMapToTheCliEnvelope) {
  FakeCli cli;
  const json check = response_of(mcp::handle_message(
      request("tools/call",
              {{"name", "ptpn_check"}, {"arguments", {{"file", "model.rtsl"}}}})
          .dump(),
      cli.runner()));
  ASSERT_EQ(1u, cli.calls.size());
  EXPECT_EQ((std::vector<std::string>{"check", "-f", "model.rtsl", "--diagnostics", "json"}),
            cli.calls.front());
  EXPECT_FALSE(check.at("result").at("isError").get<bool>());
  EXPECT_EQ("text", check.at("result").at("content").at(0).at("type").get<std::string>());
  EXPECT_EQ(0, check.at("result").at("structuredContent").at("exit_code").get<int>());

  const json explain = response_of(mcp::handle_message(
      request("tools/call",
              {{"name", "ptpn_explain"}, {"arguments", {{"code", "E-VAL-226"}}}})
          .dump(),
      cli.runner()));
  EXPECT_EQ((std::vector<std::string>{"explain", "E-VAL-226", "--diagnostics", "json"}),
            cli.calls.back());
  EXPECT_TRUE(explain.at("result").contains("structuredContent"));
}

TEST(McpTest, AnalyzeMapsBudgetsAndDetectsTheFrontEnd) {
  FakeCli cli;
  response_of(mcp::handle_message(
      request("tools/call",
              {{"name", "ptpn_analyze"},
               {"arguments", {{"file", "example/s-bench/a.json"},
                              {"timeout", 5},
                              {"max_memory", 512},
                              {"max_states", 1000},
                              {"no_exact_wcrt", true},
                              {"tie", "arbitrary"}}}})
          .dump(),
      cli.runner()));
  ASSERT_EQ(1u, cli.calls.size());
  const std::vector<std::string>& args = cli.calls.front();
  EXPECT_EQ("tdg", args.front());  // .json -> TDG front-end
  EXPECT_NE(std::find(args.begin(), args.end(), "--timeout"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "5"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "--max-memory"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "512"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "--no-exact-wcrt"), args.end());
  EXPECT_NE(std::find(args.begin(), args.end(), "--tie"), args.end());

  // A .rtsl input selects the RTSL front-end.
  response_of(mcp::handle_message(
      request("tools/call", {{"name", "ptpn_analyze"}, {"arguments", {{"file", "m.rtsl"}}}}).dump(),
      cli.runner()));
  EXPECT_EQ("rtsl", cli.calls.back().front());
}

TEST(McpTest, FillUsesTheInlineFormAndReportsToolErrors) {
  FakeCli cli;
  response_of(mcp::handle_message(
      request("tools/call",
              {{"name", "ptpn_fill"},
               {"arguments", {{"file", "model.rtsl"},
                              {"task", "fusion"},
                              {"intervals", "[1,2],[3,4],[1,2]"},
                              {"overwrite", true}}}})
          .dump(),
      cli.runner()));
  ASSERT_EQ(1u, cli.calls.size());
  EXPECT_EQ((std::vector<std::string>{"fill", "-f", "model.rtsl", "--task", "fusion",
                                      "--intervals", "[1,2],[3,4],[1,2]", "--overwrite",
                                      "--diagnostics", "json"}),
            cli.calls.front());

  // Missing required arguments are tool-level errors, not protocol errors.
  const json missing = response_of(mcp::handle_message(
      request("tools/call", {{"name", "ptpn_fill"}, {"arguments", {{"file", "model.rtsl"}}}}).dump(),
      cli.runner()));
  EXPECT_TRUE(missing.at("result").at("isError").get<bool>());
  EXPECT_NE(std::string::npos,
            missing.at("result").at("content").at(0).at("text").get<std::string>().find(
                "values or task"));

  const json unknown = response_of(mcp::handle_message(
      request("tools/call", {{"name", "nope"}, {"arguments", json::object()}}).dump(),
      cli.runner()));
  EXPECT_TRUE(unknown.at("result").at("isError").get<bool>());
}

TEST(McpTest, InternalExitCodeIsAToolErrorButAnalysisResultsAreNot) {
  FakeCli cli;
  cli.exit_code = 6;  // counterexample: a result, not a tool failure
  const json miss = response_of(mcp::handle_message(
      request("tools/call", {{"name", "ptpn_analyze"}, {"arguments", {{"file", "m.rtsl"}}}}).dump(),
      cli.runner()));
  EXPECT_FALSE(miss.at("result").at("isError").get<bool>());

  cli.exit_code = 1;  // internal failure
  const json internal = response_of(mcp::handle_message(
      request("tools/call", {{"name", "ptpn_analyze"}, {"arguments", {{"file", "m.rtsl"}}}}).dump(),
      cli.runner()));
  EXPECT_TRUE(internal.at("result").at("isError").get<bool>());
}

TEST(McpTest, ProtocolErrorsUseJsonRpcCodes) {
  FakeCli cli;
  const json parse_error = response_of(mcp::handle_message("not json", cli.runner()));
  EXPECT_EQ(-32700, parse_error.at("error").at("code").get<int>());

  const json bad_request =
      response_of(mcp::handle_message(R"({"jsonrpc":"2.0","id":3})", cli.runner()));
  EXPECT_EQ(-32600, bad_request.at("error").at("code").get<int>());
  EXPECT_EQ(3, bad_request.at("id").get<int>());

  const json not_found = response_of(mcp::handle_message(request("tools/other").dump(), cli.runner()));
  EXPECT_EQ(-32601, not_found.at("error").at("code").get<int>());
}

#if defined(__unix__) || defined(__APPLE__)
TEST(McpTest, ServesTheRealBinaryEndToEnd) {
  int to_child[2];
  int from_child[2];
  ASSERT_EQ(0, pipe(to_child));
  ASSERT_EQ(0, pipe(from_child));
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    execl(PTPN_CLI_PATH, "ptpn", "mcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  close(to_child[0]);
  close(from_child[1]);

  const std::string requests =
      request("initialize", json::object(), 1).dump() + "\n" +
      request("tools/call", {{"name", "ptpn_explain"}, {"arguments", {{"code", "E-SYN-003"}}}}, 2)
          .dump() +
      "\n";
  EXPECT_EQ(requests.size(),
            static_cast<std::size_t>(write(to_child[1], requests.data(), requests.size())));
  close(to_child[1]);

  std::string output;
  char buffer[4096];
  ssize_t read_bytes = 0;
  while ((read_bytes = read(from_child[0], buffer, sizeof(buffer))) > 0) {
    output.append(buffer, static_cast<std::size_t>(read_bytes));
  }
  close(from_child[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  EXPECT_EQ(0, ptpn_test::exit_code_from_status(status));

  std::istringstream lines(output);
  std::string line;
  ASSERT_TRUE(std::getline(lines, line));
  EXPECT_EQ("ptpn", response_of(line).at("result").at("serverInfo").at("name").get<std::string>());
  ASSERT_TRUE(std::getline(lines, line));
  const json explained = response_of(line);
  EXPECT_EQ("E-SYN-003",
            explained.at("result").at("structuredContent").at("result").at("codes").at(0).at(
                "code").get<std::string>());
}
#endif
