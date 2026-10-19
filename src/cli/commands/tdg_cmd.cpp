#include <cstddef>
#include <memory>
#include <string>

#include "app/pipelines.h"
#include "cli/command.h"
#include "cli/option_groups.h"

namespace cli {
namespace {

class TdgCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand("tdg", "Analyze from TDG JSON input");
    app_->add_option("-f,--file", input_file_, "Input TDG JSON file")->required(true);
    add_common_pipeline_options(app_, options_);
    app_->add_option("--policy", policy_,
                     "Override the TDG scheduling policy for lowering and native analysis")
        ->transform([this](const std::string& value) {
          options_.policy_override = parse_policy_option(value);
          return value;
        });
    app_->footer(
        "Examples:\n"
        "  ptpn tdg -f input.json\n"
        "  ptpn tdg -f input.json --export-scg scg.dot --export-ptpn net.dot\n"
        "  ptpn tdg -f input.json --policy fixed_prior_with_resume --export-metrics "
        "metrics.json\n"
        "  ptpn tdg -f input.json --no-analysis --export-wcet wcet.json");
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
    return app::run_tdg_pipeline(input_file_, options_, initial_memory, diagnostics);
  }

 private:
  CLI::App* app_ = nullptr;
  std::string input_file_;
  std::string policy_;
  app::PipelineOptions options_;
};

}  // namespace

std::unique_ptr<Command> make_tdg_command() {
  return std::make_unique<TdgCommand>();
}

}  // namespace cli
