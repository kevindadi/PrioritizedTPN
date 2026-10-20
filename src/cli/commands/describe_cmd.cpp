#include "app/describe.h"

#include <cstddef>
#include <memory>
#include <string>

#include "cli/command.h"
#include "cli/option_groups.h"

namespace cli {
namespace {

class DescribeCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand(
        "describe", "Restate an RTSL model's actual analysis semantics in plain text");
    app_->add_option("-f,--file", options_.input_file, "Input RTSL file")->required(true);
    app_->add_option("--policy", policy_,
                     "Describe the model as if this policy were selected (lowers and reports "
                     "the computed priorities)")
        ->transform([this](const std::string& value) {
          parse_policy_option(value);
          options_.policy_override = value;
          return value;
        });
    app_->add_option("--tie", tie_mode_,
                     "Equal-priority tie handling the description should assume (fifo or "
                     "arbitrary)")
        ->check(CLI::IsMember({"fifo", "arbitrary"}));
    add_diagnostics_options(app_, diagnostics_);
    app_->footer(
        "The description is what the analyzer will do, not what the source might\n"
        "look like: cores and priorities after policy resolution, each task's\n"
        "execution segments with lock nesting, release bindings, AND fork/join\n"
        "versus the token-consuming alternative/OR semantics of separate edge\n"
        "statements, and the validation hints that flag the common traps.\n"
        "\n"
        "Examples:\n"
        "  ptpn describe -f model.rtsl\n"
        "  ptpn describe -f model.rtsl --policy rm --tie fifo\n"
        "  ptpn describe -f model.rtsl --diagnostics json");
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

  int run(ptpn::Diagnostics* diagnostics, std::size_t /*initial_memory*/) override {
    options_.tie_mode = tie_mode_;
    return app::run_rtsl_describe(options_, diagnostics);
  }

 private:
  CLI::App* app_ = nullptr;
  app::DescribeOptions options_;
  app::DiagnosticsOptions diagnostics_;
  std::string policy_;
  std::string tie_mode_ = "fifo";
};

}  // namespace

std::unique_ptr<Command> make_describe_command() {
  return std::make_unique<DescribeCommand>();
}

}  // namespace cli
