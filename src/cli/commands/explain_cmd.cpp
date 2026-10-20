#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "cli/command.h"
#include "cli/option_groups.h"
#include "common/diagnostic_codes.h"
#include "common/diagnostics.h"

namespace cli {
namespace {

// One-line summary for the catalog listing.
std::string shorten(const std::string& text, std::size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  std::size_t cut = text.rfind(' ', limit);
  if (cut == std::string::npos || cut < limit / 2) {
    cut = limit;
  }
  return text.substr(0, cut) + "...";
}

nlohmann::json to_json(const ptpn::DiagnosticCodeInfo& info) {
  return nlohmann::json{{"code", info.code},         {"category", info.category},
                        {"severity", info.severity}, {"stage", info.stage},
                        {"trigger", info.trigger},   {"subject", info.subject},
                        {"fix", info.fix},           {"group", info.group}};
}

std::string detail(const ptpn::DiagnosticCodeInfo& info) {
  std::ostringstream out;
  out << info.code << "  [" << info.category << "/" << info.severity << "]\n";
  out << "  stage:   " << info.stage << "\n";
  out << "  trigger: " << info.trigger << "\n";
  out << "  subject: " << info.subject << "\n";
  out << "  fix:     " << info.fix << "\n";
  out << "  docs:    docs/diagnostics.md (" << info.group << ")\n";
  return out.str();
}

class ExplainCommand final : public Command {
 public:
  CLI::App* add_to(CLI::App& root) override {
    app_ = root.add_subcommand(
        "explain", "Explain diagnostic codes (no code: list the whole catalog)");
    app_->add_option("codes", codes_,
                     "One or more diagnostic codes, e.g. E-VAL-226 or R-ANA-002");
    add_diagnostics_options(app_, diagnostics_);
    app_->footer(
        "The catalog is generated from docs/diagnostics.md, so `explain` and the\n"
        "documentation cannot drift apart. Exit codes: 0 when every code is known,\n"
        "2 with E-SYN-003 for an unknown code.\n"
        "\n"
        "Examples:\n"
        "  ptpn explain\n"
        "  ptpn explain E-VAL-226\n"
        "  ptpn explain R-ANA-002 E-PERF-003\n"
        "  ptpn explain E-VAL-226 --diagnostics json");
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
    const std::vector<ptpn::DiagnosticCodeInfo>& catalog = ptpn::diagnostic_code_catalog();

    if (codes_.empty()) {
      std::ostringstream listing;
      std::string group;
      for (const ptpn::DiagnosticCodeInfo& info : catalog) {
        if (info.group != group) {
          group = info.group;
          listing << "\n" << group << "\n";
        }
        listing << "  " << info.code << "  [" << info.category << "/" << info.severity << "]  "
                << shorten(info.trigger, 78) << "\n";
      }
      const std::string text = listing.str();
      if (diagnostics != nullptr) {
        nlohmann::json codes = nlohmann::json::array();
        for (const ptpn::DiagnosticCodeInfo& info : catalog) {
          codes.push_back(to_json(info));
        }
        diagnostics->set_result(nlohmann::json{{"codes", std::move(codes)}});
        return diagnostics->exit_code();
      }
      std::cout << "diagnostic codes (docs/diagnostics.md); 'ptpn explain CODE' for details\n"
                << text;
      return 0;
    }

    nlohmann::json entries = nlohmann::json::array();
    std::vector<std::string> unknown;
    for (const std::string& code : codes_) {
      const ptpn::DiagnosticCodeInfo* info = ptpn::find_diagnostic_code(code);
      if (info == nullptr) {
        unknown.push_back(code);
        continue;
      }
      entries.push_back(to_json(*info));
    }

    if (!unknown.empty()) {
      const std::string list = [&]() {
        std::string joined;
        for (const std::string& code : unknown) {
          if (!joined.empty()) {
            joined += ", ";
          }
          joined += code;
        }
        return joined;
      }();
      if (diagnostics != nullptr) {
        diagnostics->add("E-SYN-003", "unknown diagnostic code(s): " + list,
                         "argument:" + unknown.front(), std::nullopt,
                         "run 'ptpn explain' for the full catalog");
        return diagnostics->exit_code();
      }
      std::cerr << "ERROR: E-SYN-003: unknown diagnostic code(s): " << list << '\n'
                << "  fix: run 'ptpn explain' for the full catalog\n";
      return 2;
    }

    if (diagnostics != nullptr) {
      diagnostics->set_result(nlohmann::json{{"codes", std::move(entries)}});
      return diagnostics->exit_code();
    }
    for (const std::string& code : codes_) {
      const ptpn::DiagnosticCodeInfo* info = ptpn::find_diagnostic_code(code);
      std::cout << detail(*info);
    }
    return 0;
  }

 private:
  CLI::App* app_ = nullptr;
  std::vector<std::string> codes_;
  app::DiagnosticsOptions diagnostics_;
};

}  // namespace

std::unique_ptr<Command> make_explain_command() {
  return std::make_unique<ExplainCommand>();
}

}  // namespace cli
