#include "dsl/lexer.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>

namespace dsl {

namespace {

const std::unordered_map<std::string, TokenKind>& keyword_table() {
  static const std::unordered_map<std::string, TokenKind> keywords = {
      {"system", TokenKind::KwSystem},
      {"cpus", TokenKind::KwCpus},
      {"cores_per_cpu", TokenKind::KwCoresPerCpu},
      {"time_unit", TokenKind::KwTimeUnit},
      {"policy", TokenKind::KwPolicy},
      {"task", TokenKind::KwTask},
      {"normal", TokenKind::KwNormal},
      {"periodic", TokenKind::KwPeriodic},
      {"aperiodic", TokenKind::KwAperiodic},
      {"sporadic", TokenKind::KwSporadic},
      {"interrupt", TokenKind::KwInterrupt},
      {"period", TokenKind::KwPeriod},
      {"deadline", TokenKind::KwDeadline},
      {"mit", TokenKind::KwMit},
      {"priority", TokenKind::KwPriority},
      {"core", TokenKind::KwCore},
      {"any", TokenKind::KwAny},
      {"locks", TokenKind::KwLocks},
      {"time", TokenKind::KwTime},
      {"measured", TokenKind::KwMeasured},
      {"estimated", TokenKind::KwEstimated},
      {"certified", TokenKind::KwCertified},
      {"by", TokenKind::KwBy},
      {"llm", TokenKind::KwLlm},
      {"runs", TokenKind::KwRuns},
      {"timeout", TokenKind::KwTimeout},
      {"artifact", TokenKind::KwArtifact},
      {"start", TokenKind::KwStart},
      {"end", TokenKind::KwEnd},
  };
  return keywords;
}

bool is_identifier_start(char character) {
  const unsigned char value = static_cast<unsigned char>(character);
  return std::isalpha(value) != 0 || character == '_';
}

bool is_identifier_continue(char character) {
  const unsigned char value = static_cast<unsigned char>(character);
  return std::isalnum(value) != 0 || character == '_';
}

std::string describe_character(char character) {
  const unsigned char value = static_cast<unsigned char>(character);
  if (std::isprint(value) != 0) {
    return std::string("'") + character + "'";
  }
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "'\\x%02x'", static_cast<unsigned>(value));
  return buffer;
}

}  // namespace

const char* token_kind_name(TokenKind kind) {
  switch (kind) {
    case TokenKind::EndOfFile:
      return "end of file";
    case TokenKind::Identifier:
      return "identifier";
    case TokenKind::Integer:
      return "integer";
    case TokenKind::TimeLiteral:
      return "time literal";
    case TokenKind::String:
      return "string";
    case TokenKind::Error:
      return "invalid token";
    case TokenKind::LBrace:
      return "'{'";
    case TokenKind::RBrace:
      return "'}'";
    case TokenKind::LBracket:
      return "'['";
    case TokenKind::RBracket:
      return "']'";
    case TokenKind::Comma:
      return "','";
    case TokenKind::Colon:
      return "':'";
    case TokenKind::Semicolon:
      return "';'";
    case TokenKind::Arrow:
      return "'->'";
    case TokenKind::Pipe:
      return "'|'";
    case TokenKind::KwSystem:
      return "'system'";
    case TokenKind::KwCpus:
      return "'cpus'";
    case TokenKind::KwCoresPerCpu:
      return "'cores_per_cpu'";
    case TokenKind::KwTimeUnit:
      return "'time_unit'";
    case TokenKind::KwPolicy:
      return "'policy'";
    case TokenKind::KwTask:
      return "'task'";
    case TokenKind::KwNormal:
      return "'normal'";
    case TokenKind::KwPeriodic:
      return "'periodic'";
    case TokenKind::KwAperiodic:
      return "'aperiodic'";
    case TokenKind::KwSporadic:
      return "'sporadic'";
    case TokenKind::KwInterrupt:
      return "'interrupt'";
    case TokenKind::KwPeriod:
      return "'period'";
    case TokenKind::KwDeadline:
      return "'deadline'";
    case TokenKind::KwMit:
      return "'mit'";
    case TokenKind::KwPriority:
      return "'priority'";
    case TokenKind::KwCore:
      return "'core'";
    case TokenKind::KwAny:
      return "'any'";
    case TokenKind::KwLocks:
      return "'locks'";
    case TokenKind::KwTime:
      return "'time'";
    case TokenKind::KwMeasured:
      return "'measured'";
    case TokenKind::KwEstimated:
      return "'estimated'";
    case TokenKind::KwCertified:
      return "'certified'";
    case TokenKind::KwBy:
      return "'by'";
    case TokenKind::KwLlm:
      return "'llm'";
    case TokenKind::KwRuns:
      return "'runs'";
    case TokenKind::KwTimeout:
      return "'timeout'";
    case TokenKind::KwArtifact:
      return "'artifact'";
    case TokenKind::KwStart:
      return "'start'";
    case TokenKind::KwEnd:
      return "'end'";
  }
  return "token";
}

Lexer::Lexer(std::string_view source, std::string file, ptpn::Diagnostics& diagnostics)
    : source_(source), file_(std::move(file)), diagnostics_(diagnostics) {}

char Lexer::peek(std::size_t lookahead) const {
  const std::size_t index = position_ + lookahead;
  return index < source_.size() ? source_[index] : '\0';
}

bool Lexer::at_end() const {
  return position_ >= source_.size();
}

char Lexer::advance_char() {
  if (at_end()) {
    return '\0';
  }
  const char character = source_[position_++];
  if (character == '\n') {
    ++line_;
    column_ = 1;
    line_has_token_ = false;
  } else {
    ++column_;
  }
  return character;
}

bool Lexer::match_char(char expected) {
  if (peek() != expected) {
    return false;
  }
  advance_char();
  return true;
}

Token Lexer::make_token(TokenKind kind, std::size_t start_offset, int start_line,
                        int start_column) const {
  Token token;
  token.kind = kind;
  token.text = std::string(source_.substr(start_offset, position_ - start_offset));
  token.offset = start_offset;
  token.length = position_ - start_offset;
  token.line = start_line;
  token.column = start_column;
  return token;
}

void Lexer::add_error(const std::string& message, int line, int column, std::size_t offset,
                      std::size_t length, const std::string& fix) {
  ptpn::SourceLocation location;
  location.file = file_;
  location.line = line;
  location.column = column;
  location.span = static_cast<int>(length);
  diagnostics_.add("E-SYN-101", message, std::nullopt, location, fix);
}

void Lexer::skip_whitespace_and_comments() {
  while (!at_end()) {
    const char character = peek();
    if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
      advance_char();
      continue;
    }
    if (character == '/' && peek(1) == '/') {
      advance_char();
      advance_char();
      while (!at_end() && peek() != '\n') {
        advance_char();
      }
      continue;
    }
    if (character == '/' && peek(1) == '*') {
      const std::size_t start_offset = position_;
      const int start_line = line_;
      const int start_column = column_;
      advance_char();
      advance_char();
      bool closed = false;
      while (!at_end()) {
        if (peek() == '*' && peek(1) == '/') {
          advance_char();
          advance_char();
          closed = true;
          break;
        }
        advance_char();
      }
      if (!closed) {
        add_error("unterminated block comment", start_line, start_column, start_offset,
                  position_ - start_offset, "close the comment with '*/'");
      }
      continue;
    }
    break;
  }
}

Token Lexer::lex_identifier_or_keyword() {
  const std::size_t start_offset = position_;
  const int start_line = line_;
  const int start_column = column_;
  while (!at_end() && is_identifier_continue(peek())) {
    advance_char();
  }
  Token token = make_token(TokenKind::Identifier, start_offset, start_line, start_column);
  const auto keyword = keyword_table().find(token.text);
  if (keyword != keyword_table().end()) {
    token.kind = keyword->second;
  }
  return token;
}

Token Lexer::lex_number() {
  const std::size_t start_offset = position_;
  const int start_line = line_;
  const int start_column = column_;
  while (!at_end() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
    advance_char();
  }
  const std::size_t digits_end = position_;
  std::string suffix;
  if (!at_end() && std::isalpha(static_cast<unsigned char>(peek())) != 0) {
    const std::size_t suffix_start = position_;
    while (!at_end() && is_identifier_continue(peek())) {
      advance_char();
    }
    suffix = std::string(source_.substr(suffix_start, position_ - suffix_start));
  }

  const std::string digits(source_.substr(start_offset, digits_end - start_offset));
  std::int64_t value = 0;
  const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (result.ec == std::errc::result_out_of_range) {
    value = std::numeric_limits<std::int64_t>::max();
    add_error("integer literal out of range: " + digits, start_line, start_column, start_offset,
              position_ - start_offset, "use a smaller non-negative integer");
  } else if (result.ec != std::errc()) {
    value = 0;
    add_error("invalid integer literal: " + digits, start_line, start_column, start_offset,
              position_ - start_offset, "use a decimal non-negative integer");
  }

  Token token = make_token(suffix.empty() ? TokenKind::Integer : TokenKind::TimeLiteral,
                           start_offset, start_line, start_column);
  token.int_value = value;
  token.unit = std::move(suffix);
  return token;
}

Token Lexer::lex_string() {
  const std::size_t start_offset = position_;
  const int start_line = line_;
  const int start_column = column_;
  advance_char();  // opening quote

  std::string value;
  bool closed = false;
  bool unterminated = false;
  while (!at_end()) {
    const char character = peek();
    if (character == '"') {
      advance_char();
      closed = true;
      break;
    }
    if (character == '\n') {
      unterminated = true;
      break;
    }
    if (character == '\\') {
      const int escape_line = line_;
      const int escape_column = column_;
      const std::size_t escape_offset = position_;
      advance_char();
      if (at_end()) {
        unterminated = true;
        break;
      }
      const char escaped = advance_char();
      switch (escaped) {
        case '"':
          value += '"';
          break;
        case '\\':
          value += '\\';
          break;
        case 'n':
          value += '\n';
          break;
        default:
          add_error("invalid escape sequence '\\" + std::string(1, escaped) + "'", escape_line,
                    escape_column, escape_offset, 2,
                    "use one of the supported escapes: \\\", \\\\, or \\n");
          value += escaped;
          break;
      }
      continue;
    }
    value += advance_char();
  }

  if (!closed || unterminated) {
    add_error("unterminated string literal", start_line, start_column, start_offset,
              position_ - start_offset, "terminate the string with a double quote");
    Token token = make_token(TokenKind::Error, start_offset, start_line, start_column);
    token.string_value = std::move(value);
    return token;
  }

  Token token = make_token(TokenKind::String, start_offset, start_line, start_column);
  token.string_value = std::move(value);
  return token;
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  while (true) {
    skip_whitespace_and_comments();
    if (at_end()) {
      Token eof = make_token(TokenKind::EndOfFile, position_, line_, column_);
      eof.first_on_line = !line_has_token_;
      tokens.push_back(std::move(eof));
      break;
    }

    const bool first_on_line = !line_has_token_;
    const std::size_t start_offset = position_;
    const int start_line = line_;
    const int start_column = column_;
    Token token;

    const char character = peek();
    if (is_identifier_start(character)) {
      token = lex_identifier_or_keyword();
    } else if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
      token = lex_number();
    } else if (character == '"') {
      token = lex_string();
    } else if (character == '-' && peek(1) == '>') {
      advance_char();
      advance_char();
      token = make_token(TokenKind::Arrow, start_offset, start_line, start_column);
    } else {
      switch (character) {
        case '{':
          advance_char();
          token = make_token(TokenKind::LBrace, start_offset, start_line, start_column);
          break;
        case '}':
          advance_char();
          token = make_token(TokenKind::RBrace, start_offset, start_line, start_column);
          break;
        case '[':
          advance_char();
          token = make_token(TokenKind::LBracket, start_offset, start_line, start_column);
          break;
        case ']':
          advance_char();
          token = make_token(TokenKind::RBracket, start_offset, start_line, start_column);
          break;
        case '|':
          advance_char();
          token = make_token(TokenKind::Pipe, start_offset, start_line, start_column);
          break;
        case ',':
          advance_char();
          token = make_token(TokenKind::Comma, start_offset, start_line, start_column);
          break;
        case ':':
          advance_char();
          token = make_token(TokenKind::Colon, start_offset, start_line, start_column);
          break;
        case ';':
          advance_char();
          token = make_token(TokenKind::Semicolon, start_offset, start_line, start_column);
          break;
        default:
          advance_char();
          add_error("illegal character " + describe_character(character), start_line, start_column,
                    start_offset, 1, "remove or replace the illegal character");
          token = make_token(TokenKind::Error, start_offset, start_line, start_column);
          break;
      }
    }

    token.first_on_line = first_on_line;
    line_has_token_ = true;
    tokens.push_back(std::move(token));
  }
  return tokens;
}

}  // namespace dsl
