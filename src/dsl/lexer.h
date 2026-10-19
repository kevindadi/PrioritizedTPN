#ifndef DSL_LEXER_H
#define DSL_LEXER_H

#include <string>
#include <string_view>
#include <vector>

#include "common/diagnostics.h"
#include "dsl/token.h"

namespace dsl {

// Hand-written RTSL lexer. Lexical failures are non-fatal: an Error token is
// emitted and scanning continues, which is required for panic-mode parsing.
// Diagnostics are emitted as E-SYN-101.
class Lexer {
 public:
  Lexer(std::string_view source, std::string file, ptpn::Diagnostics& diagnostics);

  std::vector<Token> tokenize();

 private:
  char peek(std::size_t lookahead = 0) const;
  bool at_end() const;
  char advance_char();
  bool match_char(char expected);

  Token make_token(TokenKind kind, std::size_t start_offset, int start_line,
                   int start_column) const;

  void skip_whitespace_and_comments();
  Token lex_identifier_or_keyword();
  Token lex_number();
  Token lex_string();
  void add_error(const std::string& message, int line, int column, std::size_t offset,
                 std::size_t length, const std::string& fix);

  std::string_view source_;
  std::string file_;
  ptpn::Diagnostics& diagnostics_;
  std::size_t position_ = 0;
  int line_ = 1;
  int column_ = 1;
  bool line_has_token_ = false;
};

}  // namespace dsl

#endif  // DSL_LEXER_H
