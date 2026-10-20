#ifndef DSL_PARSER_H
#define DSL_PARSER_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "common/diagnostics.h"
#include "dsl/ast.h"
#include "dsl/source_map.h"
#include "dsl/token.h"

namespace dsl {

// Recursive-descent RTSL parser with panic-mode recovery. Syntax failures
// emit E-SYN-102 and mark the failed task or system attribute tainted so
// validation does not report cascades. `SourceMap` is populated for every
// subject key while parsing.
class Parser {
 public:
  Parser(std::vector<Token> tokens, std::string file, ptpn::Diagnostics& diagnostics,
         SourceMap& source_map);

  ast::Program parse();

 private:
  const Token& peek(std::size_t lookahead = 0) const;
  const Token& advance();
  bool check(TokenKind kind) const;
  bool match(TokenKind kind);
  void skip_semicolons();

  [[nodiscard]] Span span_of(const Token& token) const;
  [[nodiscard]] Span span_from(const Token& first) const;

  void error_unexpected(const Token& token, const std::string& expected);
  void synchronize_top();
  void synchronize_task();
  void mark_task_tainted(const std::string& id);
  void bind_task_source_map(const ast::TaskDecl& task);

  void parse_system_item(ast::Program& program);
  void parse_system_int(std::vector<ast::Spanned<std::int64_t>>& target,
                        const std::string& subject);
  void parse_system_string(std::vector<ast::Spanned<std::string>>& target,
                           const std::string& subject);
  void parse_task(ast::Program& program);
  bool parse_task_attributes(ast::TaskDecl& task);
  bool parse_time_attr(std::vector<ast::Spanned<ast::TimeValue>>& target);
  bool parse_int_attr(std::vector<ast::Spanned<std::int64_t>>& target);
  bool parse_core_attr(ast::TaskDecl& task);
  bool parse_locks_attr(ast::TaskDecl& task);
  bool parse_time_decl(ast::TaskDecl& task);
  bool parse_intervals(ast::TimeDecl& declaration);
  bool parse_interval(ast::TimeInterval& interval);
  bool parse_time_value(ast::TimeValue& value);
  bool parse_provenance(ast::Provenance& provenance);
  bool parse_measured_block(ast::MeasureBlock& block);
  void parse_binding(ast::Program& program, ast::BindingKind kind);
  void parse_edge(ast::Program& program);
  bool parse_edge_endpoint(std::vector<ast::IdentRef>& references, bool& braced,
                           bool& alternative, const char* side);

  std::vector<Token> tokens_;
  std::string file_;
  ptpn::Diagnostics& diagnostics_;
  SourceMap& source_map_;
  std::size_t position_ = 0;
  std::size_t last_end_ = 0;
  // Number of consumed but not yet closed braces. Used by the recovery
  // routines so a `}` inside `core { ... }` or an edge set is not mistaken for
  // the enclosing block's closing brace.
  int brace_depth_ = 0;
};

}  // namespace dsl

#endif  // DSL_PARSER_H
