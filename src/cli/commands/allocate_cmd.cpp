#include <cstddef>
#include <memory>
#include <string>

#include "app/allocation_cmd.h"
#include "cli/command.h"
#include "cli/option_groups.h"

namespace cli {
namespace {

allocation::Objective parse_objective_option(const std::string& objective) {
  if (objective == "max-utilization") {
    return allocation::Objective::MAX_UTILIZATION;
  }
  if (objective == "max-load") {
    return allocation::Objective::MAX_LOAD;
  }
  return allocation::Objective::MAX_WCRT;
}

class AllocateCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand(
        "allocate", "Search deterministic core allocations with the exact PTPN analysis");
    app_->add_option("-f,--file", options_.input_file,
                     "Input TDG JSON or RTSL file (-f model.rtsl expands core any)")
        ->required(true);
    app_->add_option("-o,--output", options_.output_file, "Output allocation report JSON")
        ->required(true);
    app_->add_option("--objective", objective_,
                     "Objective to minimize: max-wcrt, max-utilization, or max-load "
                     "(default: max-wcrt)")
        ->check(CLI::IsMember({"max-wcrt", "max-utilization", "max-load"}))
        ->transform([this](const std::string& value) {
          options_.objective = parse_objective_option(value);
          return value;
        });
    app_->add_option("--max-candidates", options_.max_candidates,
                     "Maximum number of candidate assignments analyzed (default: 64)")
        ->check(CLI::Range(std::size_t{1}, std::size_t{1000000}));
    app_->add_option("--candidate-max-states", options_.candidate_max_states,
                     "State-class graph bound per candidate (default: 200000)")
        ->check(CLI::PositiveNumber);
    app_->add_flag("--require-certified-wcet", options_.require_certified_wcet,
                   "Only consider allocations whose WCET provenance is certified");
    app_->add_flag("--debug", options_.debug_mode,
                   "Enable debug logging (including per-candidate analysis logs)");
    add_diagnostics_options(app_, options_.diagnostics);
    app_->add_option("--policy", policy_,
                     "Override the TDG scheduling policy before allocation analysis")
        ->transform([this](const std::string& value) {
          options_.policy_override = parse_policy_option(value);
          return value;
        });
    app_->footer(
        "Examples:\n"
        "  ptpn allocate -f input.json -o allocation.json\n"
        "  ptpn allocate -f input.json -o allocation.json --objective max-load\n"
        "  ptpn allocate -f input.json -o allocation.json --max-candidates 32 "
        "--candidate-max-states 50000\n"
        "  ptpn allocate -f input.json -o allocation.json --require-certified-wcet\n"
        "\n"
        "The search evaluates candidate assignments with the exact state-class "
        "analysis and reports feasible and Pareto allocations. Fails (exit 1) only "
        "on input or output errors; an empty best allocation is reported in JSON.");
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

  int run(ptpn::Diagnostics* diagnostics, std::size_t /*initial_memory*/) override {
    return app::run_allocation(options_, diagnostics);
  }

 private:
  CLI::App* app_ = nullptr;
  std::string objective_ = "max-wcrt";
  std::string policy_;
  app::AllocationCommandOptions options_;
};

}  // namespace

std::unique_ptr<Command> make_allocate_command() {
  return std::make_unique<AllocateCommand>();
}

}  // namespace cli
