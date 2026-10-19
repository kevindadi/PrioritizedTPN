#include "app/check_cmd.h"

#include <cstddef>
#include <memory>
#include <string>

#include "cli/command.h"
#include "cli/option_groups.h"

namespace cli {
namespace {

class CheckCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand("check",
                               "Lex, parse, and validate an RTSL model (no lowering "
                               "or analysis)");
    app_->add_option("-f,--file", options_.input_file, "Input RTSL file")->required(true);
    add_diagnostics_options(app_, options_.diagnostics);
    app_->footer(
        "Examples:\n"
        "  ptpn check -f model.rtsl\n"
        "  ptpn check -f model.rtsl --diagnostics json\n"
        "  ptpn check -f model.rtsl --diagnostics-file diagnostics.json\n"
        "\n"
        "Successful checks report `pending_measurements` for `measured by llm`\n"
        "tasks whose intervals have not been filled in yet.");
    return app_;
  }

  [[nodiscard]] bool parsed() const override {
    return app_ != nullptr && app_->parsed();
  }

  [[nodiscard]] bool diagnostics_engaged() const override {
    return options_.diagnostics.engaged();
  }

  [[nodiscard]] bool diagnostics_stdout() const override {
    return options_.diagnostics.output == "json";
  }

  [[nodiscard]] const std::string& diagnostics_file() const override {
    return options_.diagnostics.file;
  }

  [[nodiscard]] bool debug_mode() const override {
    return false;
  }

  int run(ptpn::Diagnostics* diagnostics, std::size_t /*initial_memory*/) override {
    return app::run_rtsl_check(options_, diagnostics);
  }

 private:
  CLI::App* app_ = nullptr;
  app::CheckCommandOptions options_;
};

}  // namespace

std::unique_ptr<Command> make_check_command() {
  return std::make_unique<CheckCommand>();
}

}  // namespace cli
