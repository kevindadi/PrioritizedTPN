#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

#include "common/diagnostics.h"
#include "dsl/lexer.h"
#include "dsl/token.h"
#include "test_rtsl_helpers.h"

namespace {

using dsl::Token;
using dsl::TokenKind;

std::vector<Token> lex(const std::string& source, ptpn::Diagnostics& diagnostics) {
  return rtsl_test::lex_source(source, diagnostics);
}

TEST(RtslLexer, RecognizesKeywordsAndPunctuation) {
  ptpn::Diagnostics diagnostics;
  const std::string source =
      "system cpus cores_per_cpu time_unit policy task normal periodic aperiodic sporadic "
      "interrupt period deadline mit priority core any locks time measured estimated certified "
      "by llm runs timeout artifact start end { } [ ] , : ; ->";
  const std::vector<Token> tokens = lex(source, diagnostics);
  ASSERT_TRUE(diagnostics.empty());

  const std::vector<TokenKind> expected = {
      TokenKind::KwSystem,    TokenKind::KwCpus,      TokenKind::KwCoresPerCpu,
      TokenKind::KwTimeUnit,  TokenKind::KwPolicy,    TokenKind::KwTask,
      TokenKind::KwNormal,    TokenKind::KwPeriodic,  TokenKind::KwAperiodic,
      TokenKind::KwSporadic,  TokenKind::KwInterrupt, TokenKind::KwPeriod,
      TokenKind::KwDeadline,  TokenKind::KwMit,       TokenKind::KwPriority,
      TokenKind::KwCore,      TokenKind::KwAny,       TokenKind::KwLocks,
      TokenKind::KwTime,      TokenKind::KwMeasured,  TokenKind::KwEstimated,
      TokenKind::KwCertified, TokenKind::KwBy,        TokenKind::KwLlm,
      TokenKind::KwRuns,      TokenKind::KwTimeout,   TokenKind::KwArtifact,
      TokenKind::KwStart,     TokenKind::KwEnd,       TokenKind::LBrace,
      TokenKind::RBrace,      TokenKind::LBracket,    TokenKind::RBracket,
      TokenKind::Comma,       TokenKind::Colon,       TokenKind::Semicolon,
      TokenKind::Arrow,       TokenKind::EndOfFile,
  };
  ASSERT_EQ(tokens.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(tokens[index].kind, expected[index]) << "token " << index;
  }
}

TEST(RtslLexer, PolicyNamesAreNotKeywords) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex("policy fifo", diagnostics);
  ASSERT_TRUE(diagnostics.empty());
  ASSERT_GE(tokens.size(), 3U);
  EXPECT_EQ(tokens[0].kind, TokenKind::KwPolicy);
  EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[1].text, "fifo");
}

TEST(RtslLexer, TracksByteOffsetsLinesAndColumns) {
  ptpn::Diagnostics diagnostics;
  // "héllo" contains a two-byte UTF-8 character; positions are counted in
  // bytes, not code points.
  const std::string source = "// h\xc3\xa9llo\n  system s";
  const std::vector<Token> tokens = lex(source, diagnostics);
  ASSERT_TRUE(diagnostics.empty());
  ASSERT_GE(tokens.size(), 3U);

  EXPECT_EQ(tokens[0].kind, TokenKind::KwSystem);
  EXPECT_EQ(tokens[0].offset, 12U);
  EXPECT_EQ(tokens[0].line, 2);
  EXPECT_EQ(tokens[0].column, 3);
  EXPECT_TRUE(tokens[0].first_on_line);

  EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[1].offset, 19U);
  EXPECT_EQ(tokens[1].line, 2);
  EXPECT_EQ(tokens[1].column, 10);
  EXPECT_FALSE(tokens[1].first_on_line);

  EXPECT_EQ(tokens[2].kind, TokenKind::EndOfFile);
  EXPECT_EQ(tokens[2].offset, source.size());
}

TEST(RtslLexer, NewLineAfterCommentStartsALine) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex("// note\nsystem", diagnostics);
  ASSERT_TRUE(diagnostics.empty());
  ASSERT_GE(tokens.size(), 2U);
  EXPECT_TRUE(tokens[0].first_on_line);
  EXPECT_EQ(tokens[0].line, 2);
}

TEST(RtslLexer, ParsesIntegerAndTimeLiterals) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex("100ms 2s 7us 3foo 42", diagnostics);
  ASSERT_TRUE(diagnostics.empty());
  ASSERT_GE(tokens.size(), 6U);

  EXPECT_EQ(tokens[0].kind, TokenKind::TimeLiteral);
  EXPECT_EQ(tokens[0].int_value, 100);
  EXPECT_EQ(tokens[0].unit, "ms");

  EXPECT_EQ(tokens[1].kind, TokenKind::TimeLiteral);
  EXPECT_EQ(tokens[1].int_value, 2);
  EXPECT_EQ(tokens[1].unit, "s");

  EXPECT_EQ(tokens[2].kind, TokenKind::TimeLiteral);
  EXPECT_EQ(tokens[2].int_value, 7);
  EXPECT_EQ(tokens[2].unit, "us");

  EXPECT_EQ(tokens[3].kind, TokenKind::TimeLiteral);
  EXPECT_EQ(tokens[3].int_value, 3);
  EXPECT_EQ(tokens[3].unit, "foo");

  EXPECT_EQ(tokens[4].kind, TokenKind::Integer);
  EXPECT_EQ(tokens[4].int_value, 42);
  EXPECT_TRUE(tokens[4].unit.empty());
}

TEST(RtslLexer, DecodesStringEscapes) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex(R"("a\"b\\c\nd")", diagnostics);
  ASSERT_TRUE(diagnostics.empty());
  ASSERT_GE(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].kind, TokenKind::String);
  EXPECT_EQ(tokens[0].string_value, "a\"b\\c\nd");
  EXPECT_EQ(tokens[0].length, 12U);
}

TEST(RtslLexer, ReportsInvalidEscapeButKeepsString) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex(R"("a\q")", diagnostics);
  const ptpn::Diagnostic* error = rtsl_test::find_diagnostic(diagnostics, "E-SYN-101");
  ASSERT_NE(error, nullptr);
  EXPECT_NE(error->message.find("invalid escape sequence"), std::string::npos);
  ASSERT_GE(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].kind, TokenKind::String);
  EXPECT_EQ(tokens[0].string_value, "aq");
}

TEST(RtslLexer, SkipsLineAndBlockComments) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens =
      lex("system /* block\ncomment */ s // tail\ncpus 1", diagnostics);
  ASSERT_TRUE(diagnostics.empty());
  ASSERT_GE(tokens.size(), 5U);
  EXPECT_EQ(tokens[0].kind, TokenKind::KwSystem);
  EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[1].text, "s");
  EXPECT_EQ(tokens[2].kind, TokenKind::KwCpus);
  EXPECT_EQ(tokens[3].kind, TokenKind::Integer);
}

TEST(RtslLexer, ReportsUnterminatedBlockComment) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex("/* still open", diagnostics);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-SYN-101"), 1U);
  ASSERT_GE(tokens.size(), 1U);
  EXPECT_EQ(tokens.back().kind, TokenKind::EndOfFile);
}

TEST(RtslLexer, IllegalCharacterProducesErrorTokenAndContinues) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex("@ system", diagnostics);
  const ptpn::Diagnostic* error = rtsl_test::find_diagnostic(diagnostics, "E-SYN-101");
  ASSERT_NE(error, nullptr);
  ASSERT_TRUE(error->location.has_value());
  EXPECT_EQ(error->location->line, 1);
  EXPECT_EQ(error->location->column, 1);
  EXPECT_EQ(error->location->span, 1);
  EXPECT_NE(error->message.find("illegal character '@'"), std::string::npos);

  ASSERT_GE(tokens.size(), 3U);
  EXPECT_EQ(tokens[0].kind, TokenKind::Error);
  EXPECT_EQ(tokens[0].offset, 0U);
  EXPECT_EQ(tokens[0].length, 1U);
  EXPECT_EQ(tokens[1].kind, TokenKind::KwSystem);
  EXPECT_EQ(tokens[1].column, 3);
}

TEST(RtslLexer, UnterminatedStringStopsAtNewlineAndResumes) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex("\"abc\nnext", diagnostics);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-SYN-101"), 1U);
  ASSERT_GE(tokens.size(), 3U);
  EXPECT_EQ(tokens[0].kind, TokenKind::Error);
  EXPECT_EQ(tokens[0].offset, 0U);
  EXPECT_EQ(tokens[0].length, 4U);
  EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[1].text, "next");
  EXPECT_EQ(tokens[1].line, 2);
  EXPECT_EQ(tokens[1].column, 1);
}

TEST(RtslLexer, OutOfRangeIntegerSaturates) {
  ptpn::Diagnostics diagnostics;
  const std::vector<Token> tokens = lex("99999999999999999999", diagnostics);
  EXPECT_EQ(rtsl_test::count_code(diagnostics, "E-SYN-101"), 1U);
  ASSERT_GE(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].kind, TokenKind::Integer);
  EXPECT_EQ(tokens[0].int_value, std::numeric_limits<std::int64_t>::max());
}

}  // namespace
