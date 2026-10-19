#include <cstddef>
#include <memory>
#include <string>

#include "app/pipeline.h"
#include "cli/command.h"
#include "cli/option_groups.h"

namespace cli {
namespace {

class PtpnCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand("ptpn", "Analyze from PTPN source file");
    app_->add_option("-f,--file", input_file_, "Input .ptpn source file")->required(true);
    add_common_pipeline_options(app_, options_);
    app_->footer(
        "Examples:\n"
        "  ptpn ptpn -f model.ptpn\n"
        "  ptpn ptpn -f model.ptpn --export-scg scg.dot --canonicalization "
        "max-lower\n"
        "  ptpn ptpn -f model.ptpn --no-analysis --export-ptpn net.dot");
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
    return options_.debug_mode;
  }

  int run(ptpn::Diagnostics* diagnostics, std::size_t initial_memory) override {
    return app::run_ptpn_pipeline(input_file_, options_, initial_memory, diagnostics);
  }

 private:
  CLI::App* app_ = nullptr;
  std::string input_file_;
  app::PipelineOptions options_;
};

}  // namespace

std::unique_ptr<Command> make_ptpn_command() {
  return std::make_unique<PtpnCommand>();
}

}  // namespace cli
