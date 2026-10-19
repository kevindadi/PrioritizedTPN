#include <CLI/CLI.hpp>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <memory>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

#include "app/pipeline.h"
#include "cli/command.h"
#include "common/diagnostics.h"

namespace {

// When the diagnostics envelope is active, stdout carries only the envelope;
// human-readable logs move to stderr (the [STATS] line format is unchanged).
void redirect_logs_to_stderr() {
  auto logger = spdlog::stderr_color_mt("ptpn-diagnostics");
  spdlog::set_default_logger(std::move(logger));
}

int run_cli(int argc, char* argv[], ptpn::Diagnostics& diagnostics, bool& diagnostics_engaged,
            bool& diagnostics_stdout, std::string& diagnostics_file, std::streambuf*& saved_cout) {
  CLI::App app{"PTPN - Priority Timed Petri Net Analyzer"};
  app.set_version_flag("-v,--version", "1.0.0");
  app.require_subcommand(1);
  app.footer(
      "Usage:\n"
      "  ptpn tdg -f <input.json> [--export-scg graph.dot] ...\n"
      "  ptpn rtsl -f <model.rtsl> [--export-tdg lowered.json] ...\n"
      "  ptpn ptpn -f <model.ptpn> [--export-ptpn net.dot] ...\n"
      "  ptpn allocate -f <input.json|model.rtsl> -o <report.json> [--objective ...]\n"
      "  ptpn check -f <model.rtsl> [--diagnostics json]\n"
      "\n"
      "File exports are opt-in. Run 'ptpn <subcommand> -h' for details.");

  std::vector<std::unique_ptr<cli::Command>> commands;
  commands.push_back(cli::make_tdg_command());
  commands.push_back(cli::make_rtsl_command());
  commands.push_back(cli::make_ptpn_command());
  commands.push_back(cli::make_allocate_command());
  commands.push_back(cli::make_check_command());
  for (const auto& command : commands) {
    command->add_to(app);
  }

  CLI11_PARSE(app, argc, argv);

  cli::Command* selected = nullptr;
  for (const auto& command : commands) {
    if (command->parsed()) {
      selected = command.get();
      break;
    }
  }
  if (selected == nullptr) {
    return 1;
  }

  diagnostics_engaged = selected->diagnostics_engaged();
  diagnostics_stdout = selected->diagnostics_stdout();
  diagnostics_file = selected->diagnostics_file();
  if (diagnostics_engaged) {
    // Keep stdout reserved for the envelope; logs (including [STATS]) and any
    // stray direct stdout writes are redirected to stderr.
    redirect_logs_to_stderr();
    saved_cout = std::cout.rdbuf(std::cerr.rdbuf());
  }

  if (selected->debug_mode()) {
    spdlog::set_level(spdlog::level::debug);
    spdlog::debug("[MAIN] Debug logging enabled");
  }

  spdlog::info("==========================================");
  spdlog::info("PTPN - Priority Timed Petri Net Analyzer");
  spdlog::info("==========================================");

  const size_t initial_memory = app::get_memory_usage();
  ptpn::Diagnostics* diag = diagnostics_engaged ? &diagnostics : nullptr;
  return selected->run(diag, initial_memory);
}

}  // namespace

int main(int argc, char* argv[]) {
  ptpn::Diagnostics diagnostics;
  bool diagnostics_engaged = false;
  bool diagnostics_stdout = false;
  std::string diagnostics_file;
  std::streambuf* saved_cout = nullptr;
  int result_code = 1;

  try {
    result_code = run_cli(argc, argv, diagnostics, diagnostics_engaged, diagnostics_stdout,
                          diagnostics_file, saved_cout);
  } catch (const std::exception& e) {
    if (diagnostics_engaged) {
      diagnostics.add("E-INT-001", e.what(), "system:internal", std::nullopt,
                      "report this as an internal error");
    }
    std::cerr << "ERROR: " << e.what() << std::endl;
    result_code = 1;
  } catch (...) {
    if (diagnostics_engaged) {
      diagnostics.add("E-INT-001", "unknown internal error", "system:internal", std::nullopt,
                      "report this as an internal error");
    }
    std::cerr << "ERROR: unknown internal error" << std::endl;
    result_code = 1;
  }

  if (saved_cout != nullptr) {
    std::cout.rdbuf(saved_cout);
  }

  if (!diagnostics_engaged) {
    return result_code;
  }

  // The envelope is the machine-readable outcome: failures without a specific
  // diagnostic still surface as an internal error rather than a false success.
  if (result_code != 0 && diagnostics.exit_code() == 0) {
    diagnostics.add("E-INT-001", "operation failed with exit code " + std::to_string(result_code),
                    "system:internal", std::nullopt,
                    "report this failure; it has no specific diagnostic");
  }

  // Open the file destination before serializing so a write failure is part of
  // the same envelope that would have been written.
  std::ofstream envelope_file;
  if (!diagnostics_file.empty()) {
    envelope_file.open(diagnostics_file);
    if (!envelope_file) {
      std::cerr << "ERROR: Failed to write diagnostics file: " << diagnostics_file << std::endl;
      diagnostics.add("E-INT-002", "failed to write diagnostics file: " + diagnostics_file,
                      "output:" + diagnostics_file, std::nullopt,
                      "check that the output path is writable");
    }
  }

  int code = diagnostics.exit_code();
  const nlohmann::json envelope = diagnostics.to_json();
  if (diagnostics_stdout) {
    std::cout << envelope.dump(2) << std::endl;
  }
  if (envelope_file) {
    envelope_file << envelope.dump(2) << std::endl;
  }
  return code;
}
