#include <cstddef>
#include <gtest/gtest.h>
#include <random>
#include <spdlog/spdlog.h>
#include <string>

#include "parse/json.h"
#include "parse/ptpn_parser.h"

namespace {

class ScopedLogSilencer {
 public:
  ScopedLogSilencer() : previous_(spdlog::get_level()) {
    spdlog::set_level(spdlog::level::off);
  }

  ~ScopedLogSilencer() {
    spdlog::set_level(previous_);
  }

 private:
  spdlog::level::level_enum previous_;
};

std::string random_text(std::mt19937& rng, size_t length) {
  static const char kAlphabet[] =
      "{}[]\":,0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-. \n";
  std::uniform_int_distribution<size_t> pick(0, sizeof(kAlphabet) - 2);
  std::string text;
  text.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    text += kAlphabet[pick(rng)];
  }
  return text;
}

}  // namespace

TEST(ParserRobustnessTest, JsonParserNeverThrowsOnGarbage) {
  ScopedLogSilencer silence;
  std::mt19937 rng(12345);
  std::uniform_int_distribution<size_t> length_dist(0, 200);

  for (int i = 0; i < 1000; ++i) {
    const std::string input = random_text(rng, length_dist(rng));
    parse::Parser parser;
    parse::ParseResult result;
    ASSERT_NO_THROW(result = parser.parse_string(input));
    if (result.success) {
      // Accidentally valid input must still survive semantic validation.
      ASSERT_NO_THROW(parser.validate());
    }
  }
}

TEST(ParserRobustnessTest, JsonParserNeverThrowsOnMutations) {
  ScopedLogSilencer silence;
  const std::string valid = R"({
    "graph": {"name": "Robust"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 1, "policy": "fixed"},
    "nodes": [
      {"id": "A", "type": "task", "priority": 1, "core": 0, "time": [[1, 2]], "locks": []},
      {"id": "B", "type": "task", "priority": 2, "core": 1, "time": [[2, 3]], "locks": []}
    ],
    "edges": [{"source": "A", "target": "B", "label": "1"}]
  })";

  std::mt19937 rng(6789);
  std::uniform_int_distribution<size_t> pos_dist(0, valid.size() - 1);
  std::uniform_int_distribution<int> char_dist(32, 126);

  for (int i = 0; i < 1000; ++i) {
    std::string mutated = valid;
    const size_t edits = 1 + static_cast<size_t>(rng() % 4);
    for (size_t e = 0; e < edits; ++e) {
      mutated[pos_dist(rng)] = static_cast<char>(char_dist(rng));
    }

    parse::Parser parser;
    parse::ParseResult result;
    ASSERT_NO_THROW(result = parser.parse_string(mutated));
    if (result.success) {
      ASSERT_NO_THROW(parser.validate());
    }
  }
}

TEST(ParserRobustnessTest, PtpnParserNeverThrowsOnGarbage) {
  ScopedLogSilencer silence;
  std::mt19937 rng(24680);
  std::uniform_int_distribution<size_t> length_dist(0, 200);

  for (int i = 0; i < 1000; ++i) {
    const std::string input = random_text(rng, length_dist(rng));

    parser::PTPNAST ast;
    std::string error;
    bool parsed = false;
    ASSERT_NO_THROW(parsed = parser::PTPNParser::parse(input, ast, error));
    (void)parsed;

    ASSERT_NO_THROW(parser::PTPNBuilder::parse(input));
  }
}
