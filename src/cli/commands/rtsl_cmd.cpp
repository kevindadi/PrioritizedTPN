#include <cstddef>
#include <memory>
#include <string>

#include "app/pipeline.h"
#include "cli/command.h"
#include "cli/option_groups.h"

namespace cli {
namespace {

class RtslCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand("rtsl", "Analyze an RTSL model end to end");
    app_->add_option("-f,--file", input_file_, "Input RTSL file")->required(true);
    add_common_pipeline_options(app_, options_, /*tdg_export_is_json=*/true);
    app_->add_option("--policy", policy_,
                     "Override the RTSL scheduling policy for lowering and native analysis")
        ->transform([this](const std::string& value) {
          options_.policy_override = parse_policy_option(value);
          return value;
        });
    app_->footer(
        "Examples:\n"
        "  ptpn rtsl -f model.rtsl\n"
        "  ptpn rtsl -f model.rtsl --export-tdg lowered.json --export-metrics "
        "metrics.json\n"
        "  ptpn rtsl -f model.rtsl --diagnostics json\n"
        "  ptpn rtsl -f model.rtsl --policy fixed_prior_with_resume --export-scg "
        "scg.dot\n"
        "\n"
        "Lowering runs in process; `--export-tdg` materializes the TDG JSON\n"
        "that `ptpn tdg` consumes. Models using `core any` require `ptpn\n"
        "allocate` (E-VAL-218).");
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
    return app::run_rtsl_pipeline(input_file_, options_, initial_memory, diagnostics);
  }

 private:
  CLI::App* app_ = nullptr;
  std::string input_file_;
  std::string policy_;
  app::PipelineOptions options_;
};

}  // namespace

std::unique_ptr<Command> make_rtsl_command() {
  return std::make_unique<RtslCommand>();
}

}  // namespace cli
