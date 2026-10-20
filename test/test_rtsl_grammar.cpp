#include <cstddef>
#include <fstream>
#include <regex>
#include <set>
#include <string>

#include <gtest/gtest.h>

namespace {

// Reserved words from the lexer's keyword table (src/dsl/lexer.cpp), the
// single source of truth for what RTSL may not use as an identifier.
std::set<std::string> lexer_keywords() {
  std::ifstream input("src/dsl/lexer.cpp");
  EXPECT_TRUE(input) << "cannot open src/dsl/lexer.cpp";
  const std::regex entry("\\{\"([a-z_]+)\", TokenKind::Kw");
  std::set<std::string> keywords;
  std::string line;
  while (std::getline(input, line)) {
    std::smatch match;
    if (std::regex_search(line, match, entry)) {
      keywords.insert(match[1].str());
    }
  }
  return keywords;
}

// The `keyword` production of docs/rtsl.ebnf: every quoted terminal from that
// production onwards (the production is the last one in the file).
std::set<std::string> grammar_keywords() {
  std::ifstream input("docs/rtsl.ebnf");
  EXPECT_TRUE(input) << "cannot open docs/rtsl.ebnf";
  const std::regex terminal(R"re("([a-z_]+)")re");
  std::set<std::string> keywords;
  bool in_keyword_production = false;
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("keyword", 0) == 0) {
      in_keyword_production = true;
    }
    if (!in_keyword_production) {
      continue;
    }
    for (std::sregex_iterator it(line.begin(), line.end(), terminal), end; it != end; ++it) {
      keywords.insert((*it)[1].str());
    }
    if (line.find(';') != std::string::npos) {
      in_keyword_production = false;
    }
  }
  return keywords;
}

}  // namespace

TEST(RtslGrammarTest, ReservedWordsMatchTheLexer) {
  const std::set<std::string> lexer = lexer_keywords();
  ASSERT_FALSE(lexer.empty());
  const std::set<std::string> grammar = grammar_keywords();
  ASSERT_FALSE(grammar.empty());

  for (const std::string& keyword : lexer) {
    EXPECT_TRUE(grammar.count(keyword) != 0)
        << "keyword '" << keyword << "' is missing from docs/rtsl.ebnf";
  }
  for (const std::string& keyword : grammar) {
    EXPECT_TRUE(lexer.count(keyword) != 0)
        << "'" << keyword << "' is reserved in docs/rtsl.ebnf but not in the lexer";
  }
}

TEST(RtslGrammarTest, GrammarMentionsEveryProduction) {
  std::ifstream input("docs/rtsl.ebnf");
  ASSERT_TRUE(input) << "cannot open docs/rtsl.ebnf";
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  for (const char* production : {"program", "system_item", "task_decl", "task_attr", "time_attr",
                                 "intervals", "provenance", "measure_item", "edge_stmt",
                                 "endpoint", "time_value", "identifier", "string", "keyword"}) {
    EXPECT_NE(std::string::npos, text.find(std::string(production) + " "))
        << "production '" << production << "' is missing";
  }
}
