#ifndef DSL_TOKEN_H
#define DSL_TOKEN_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace dsl {

// Lexical token kinds for RTSL. Policy names are deliberately not keywords:
// they lex as Identifier and are validated semantically via
// parse_schedule_policy().
enum class TokenKind {
  EndOfFile,
  Identifier,
  Integer,
  // INT with an alphabetic suffix. The suffix is not classified here: `us` and
  // unknown units become semantic diagnostics (E-UNS-004 / E-SYN-101).
  TimeLiteral,
  String,
  // Lexical failure (illegal character, unterminated string). The lexer has
  // already emitted the corresponding E-SYN-101 diagnostic; the parser treats
  // the token as a recovery point.
  Error,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Colon,
  Semicolon,
  Arrow,
  KwSystem,
  KwCpus,
  KwCoresPerCpu,
  KwTimeUnit,
  KwPolicy,
  KwTask,
  KwNormal,
  KwPeriodic,
  KwAperiodic,
  KwSporadic,
  KwInterrupt,
  KwPeriod,
  KwDeadline,
  KwMit,
  KwPriority,
  KwCore,
  KwAny,
  KwLocks,
  KwTime,
  KwMeasured,
  KwEstimated,
  KwCertified,
  KwBy,
  KwLlm,
  KwRuns,
  KwTimeout,
  KwArtifact,
  KwStart,
  KwEnd,
};

// A source token with byte-accurate coordinates. `offset` is the byte offset
// from the start of the file; `line` and `column` are 1-based and counted in
// bytes, matching the byte-slice replacement contract. `first_on_line` is true
// when no other token precedes this one on its line (comments and whitespace
// do not count), which the parser uses for top-level panic-mode
// synchronization.
struct Token {
  TokenKind kind = TokenKind::EndOfFile;
  std::string text;
  std::size_t offset = 0;
  std::size_t length = 0;
  int line = 0;
  int column = 0;
  bool first_on_line = false;
  // Numeric payload for Integer and TimeLiteral.
  std::int64_t int_value = 0;
  // TimeLiteral suffix as written ("ms", "s", "us", or an unknown suffix).
  std::string unit;
  // Unescaped contents for String tokens.
  std::string string_value;

  [[nodiscard]] std::size_t end_offset() const {
    return offset + length;
  }
};

// Human-readable name used in syntax diagnostics.
const char* token_kind_name(TokenKind kind);

}  // namespace dsl

#endif  // DSL_TOKEN_H
