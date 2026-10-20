#include "app/fill.h"

#include <cstddef>
#include <memory>
#include <string>

#include "cli/command.h"
#include "cli/option_groups.h"

namespace cli {
namespace {

class FillCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand(
        "fill", "Fill pending `measured by llm` timing into the RTSL source");
    app_->add_option("-f,--file", options_.input_file, "Input RTSL file")->required(true);
    app_->add_option("--values", options_.values_file,
                     "JSON object mapping task names to interval lists: {\"task\": [[min, max], "
                     "...]} (engine ticks, i.e. milliseconds)");
    app_->add_option("--task", options_.task, "Fill a single task instead of --values");
    app_->add_option("--intervals", options_.intervals,
                     "Intervals for --task, e.g. \"[1,2],[3,4],[1,2]\"");
    app_->add_option("-o,--output", options_.output_file,
                     "Write the filled source here instead of stdout");
    app_->add_flag("--in-place", options_.in_place,
                   "Rewrite the input file atomically (temporary file + rename)");
    app_->add_flag("--overwrite", options_.overwrite,
                   "Also replace tasks whose time attribute already has intervals");
    add_diagnostics_options(app_, diagnostics_);
    app_->footer(
        "Each pending task needs exactly 2*lock_count+1 intervals (pre-critical,\n"
        "one critical section per lock, post-critical); `ptpn check` lists the\n"
        "expected counts. The provenance clause (`measured by llm { runs ... }`)\n"
        "is preserved verbatim, and the patched source is re-validated before it\n"
        "is written, so fill never produces an invalid model.\n"
        "\n"
        "Examples:\n"
        "  ptpn fill -f model.rtsl --values times.json --in-place\n"
        "  ptpn fill -f model.rtsl --task fusion --intervals \"[1,2],[3,4],[1,2]\"\n"
        "  ptpn fill -f model.rtsl --values times.json -o filled.rtsl\n"
        "  ptpn fill -f model.rtsl --values times.json --diagnostics json");
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

  [[nodiscard]] bool logs_to_stderr() const override {
    return true;
  }

  int run(ptpn::Diagnostics* diagnostics, std::size_t /*initial_memory*/) override {
    return app::run_rtsl_fill(options_, diagnostics);
  }

 private:
  CLI::App* app_ = nullptr;
  app::FillOptions options_;
  app::DiagnosticsOptions diagnostics_;
};

}  // namespace

std::unique_ptr<Command> make_fill_command() {
  return std::make_unique<FillCommand>();
}

}  // namespace cli
