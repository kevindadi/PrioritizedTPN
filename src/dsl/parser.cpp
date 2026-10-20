#include "dsl/parser.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace dsl {

namespace {

bool is_top_level_keyword(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwCpus:
    case TokenKind::KwCoresPerCpu:
    case TokenKind::KwTimeUnit:
    case TokenKind::KwPolicy:
    case TokenKind::KwTask:
    case TokenKind::KwStart:
    case TokenKind::KwEnd:
      return true;
    default:
      return false;
  }
}

bool is_task_attr_keyword(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwPeriod:
    case TokenKind::KwDeadline:
    case TokenKind::KwMit:
    case TokenKind::KwPriority:
    case TokenKind::KwCore:
    case TokenKind::KwLocks:
    case TokenKind::KwTime:
      return true;
    default:
      return false;
  }
}

std::optional<ast::TaskKind> task_kind_for(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwNormal:
      return ast::TaskKind::Normal;
    case TokenKind::KwPeriodic:
      return ast::TaskKind::Periodic;
    case TokenKind::KwAperiodic:
      return ast::TaskKind::Aperiodic;
    case TokenKind::KwSporadic:
      return ast::TaskKind::Sporadic;
    case TokenKind::KwInterrupt:
      return ast::TaskKind::Interrupt;
    default:
      return std::nullopt;
  }
}

}  // namespace

Parser::Parser(std::vector<Token> tokens, std::string file, ptpn::Diagnostics& diagnostics,
               SourceMap& source_map)
    : tokens_(std::move(tokens)),
      file_(std::move(file)),
      diagnostics_(diagnostics),
      source_map_(source_map) {}

const Token& Parser::peek(std::size_t lookahead) const {
  const std::size_t index = std::min(position_ + lookahead, tokens_.size() - 1);
  return tokens_[index];
}

const Token& Parser::advance() {
  const Token& token = tokens_[position_];
  if (token.kind == TokenKind::LBrace) {
    ++brace_depth_;
  } else if (token.kind == TokenKind::RBrace && brace_depth_ > 0) {
    --brace_depth_;
  }
  last_end_ = token.end_offset();
  if (position_ + 1 < tokens_.size()) {
    ++position_;
  }
  return token;
}

bool Parser::check(TokenKind kind) const {
  return peek().kind == kind;
}

bool Parser::match(TokenKind kind) {
  if (!check(kind)) {
    return false;
  }
  advance();
  return true;
}

void Parser::skip_semicolons() {
  while (match(TokenKind::Semicolon)) {}
}

Span Parser::span_of(const Token& token) const {
  return Span{file_, token.offset, token.end_offset(), token.line, token.column};
}

Span Parser::span_from(const Token& first) const {
  return Span{file_, first.offset, last_end_, first.line, first.column};
}

void Parser::error_unexpected(const Token& token, const std::string& expected) {
  // Lexical failures already carry E-SYN-101; do not add a duplicate syntax
  // diagnostic for the same byte.
  if (token.kind == TokenKind::Error) {
    return;
  }
  std::string message;
  if (token.kind == TokenKind::EndOfFile) {
    message = "unexpected end of file; expected " + expected;
  } else {
    message = "unexpected token '" + token.text + "'; expected " + expected;
  }
  ptpn::SourceLocation location;
  location.file = file_;
  location.line = token.line;
  location.column = token.column;
  location.span = static_cast<int>(token.length);
  diagnostics_.add("E-SYN-102", std::move(message), std::nullopt, location,
                   "rewrite this line using the expected keywords");
}

void Parser::synchronize_top() {
  // Top-level sync points (section 3.1): system/top-level keywords and the
  // first token of a new line (IDENT or '{'). Only the closing '}' is
  // depth-aware, so a '}' inside an edge set is consumed instead of ending the
  // system block.
  while (!check(TokenKind::EndOfFile)) {
    const Token& token = peek();
    if (is_top_level_keyword(token.kind)) {
      return;
    }
    if (token.first_on_line &&
        (token.kind == TokenKind::Identifier || token.kind == TokenKind::LBrace)) {
      return;
    }
    if (token.kind == TokenKind::RBrace && brace_depth_ <= 1) {
      return;
    }
    advance();
  }
}

void Parser::synchronize_task() {
  // Task-block sync points: attribute keywords and the task's closing brace
  // (depth-aware: braces consumed by core sets are not sync points).
  while (!check(TokenKind::EndOfFile)) {
    if (is_task_attr_keyword(peek().kind)) {
      return;
    }
    if (check(TokenKind::RBrace) && brace_depth_ <= 2) {
      return;
    }
    advance();
  }
}

void Parser::mark_task_tainted(const std::string& id) {
  diagnostics_.mark_tainted("task:" + id);
  diagnostics_.mark_tainted("task:" + id + ":core");
  diagnostics_.mark_tainted("task:" + id + ":time");
  diagnostics_.mark_tainted("task:" + id + ":period");
  diagnostics_.mark_tainted("task:" + id + ":deadline");
  diagnostics_.mark_tainted("task:" + id + ":mit");
  diagnostics_.mark_tainted("task:" + id + ":priority");
}

void Parser::bind_task_source_map(const ast::TaskDecl& task) {
  const std::string prefix = "task:" + task.id;
  source_map_.bind(prefix, task.span);
  if (!task.period.empty()) {
    source_map_.bind(prefix + ":period", task.period.front().span);
  }
  if (!task.deadline.empty()) {
    source_map_.bind(prefix + ":deadline", task.deadline.front().span);
  }
  if (!task.mit.empty()) {
    source_map_.bind(prefix + ":mit", task.mit.front().span);
  }
  if (!task.priority.empty()) {
    source_map_.bind(prefix + ":priority", task.priority.front().span);
  }
  if (!task.core.empty()) {
    source_map_.bind(prefix + ":core", task.core.front().span);
  }
  if (!task.time.empty()) {
    source_map_.bind(prefix + ":time", task.time.front().span);
  }
}

ast::Program Parser::parse() {
  ast::Program program;
  skip_semicolons();

  if (!check(TokenKind::KwSystem)) {
    error_unexpected(peek(), "'system' at the start of the file");
    while (!check(TokenKind::EndOfFile) && !check(TokenKind::KwSystem)) {
      advance();
    }
    if (check(TokenKind::EndOfFile)) {
      return program;
    }
  }

  const Token& system_keyword = advance();
  if (check(TokenKind::Identifier)) {
    const Token& name = advance();
    program.system.name = name.text;
    program.system.name_span = span_of(name);
  } else {
    error_unexpected(peek(), "an identifier after 'system'");
  }

  if (!match(TokenKind::LBrace)) {
    error_unexpected(peek(), "'{' to open the system block");
    while (!check(TokenKind::EndOfFile) && !check(TokenKind::LBrace) &&
           !is_top_level_keyword(peek().kind)) {
      advance();
    }
    if (!match(TokenKind::LBrace)) {
      // Without a block there is nothing semantic to validate; suppress the
      // missing-system-attribute cascade.
      diagnostics_.mark_tainted("system:cpus");
      diagnostics_.mark_tainted("system:cores_per_cpu");
      diagnostics_.mark_tainted("system:time_unit");
      diagnostics_.mark_tainted("policy");
      program.system.span = span_from(system_keyword);
      return program;
    }
  }

  while (!check(TokenKind::EndOfFile) && !check(TokenKind::RBrace)) {
    skip_semicolons();
    if (check(TokenKind::EndOfFile) || check(TokenKind::RBrace)) {
      break;
    }
    if (check(TokenKind::Error)) {
      advance();  // E-SYN-101 already emitted by the lexer.
      continue;
    }
    parse_system_item(program);
  }

  if (check(TokenKind::RBrace)) {
    const Token& closing = advance();
    program.system.span = Span{file_, system_keyword.offset, closing.end_offset(),
                               system_keyword.line, system_keyword.column};
    // Trailing content after the system block used to be silently ignored;
    // report it instead so a stray declaration cannot pass validation.
    skip_semicolons();
    if (!check(TokenKind::EndOfFile)) {
      error_unexpected(peek(), "end of file after the system block");
      while (!check(TokenKind::EndOfFile)) {
        advance();
      }
    }
  } else {
    error_unexpected(peek(), "'}' to close the system block");
    program.system.span = span_from(system_keyword);
  }
  return program;
}

void Parser::parse_system_item(ast::Program& program) {
  switch (peek().kind) {
    case TokenKind::KwCpus:
      parse_system_int(program.system.cpus, "system:cpus");
      break;
    case TokenKind::KwCoresPerCpu:
      parse_system_int(program.system.cores_per_cpu, "system:cores_per_cpu");
      break;
    case TokenKind::KwTimeUnit:
      parse_system_string(program.system.time_unit, "system:time_unit");
      break;
    case TokenKind::KwPolicy:
      parse_system_string(program.system.policy, "policy");
      break;
    case TokenKind::KwTask:
      parse_task(program);
      break;
    case TokenKind::KwStart:
      parse_binding(program, ast::BindingKind::Start);
      break;
    case TokenKind::KwEnd:
      parse_binding(program, ast::BindingKind::End);
      break;
    case TokenKind::Identifier:
    case TokenKind::LBrace:
      parse_edge(program);
      break;
    default:
      error_unexpected(peek(),
                       "a system item ('cpus', 'cores_per_cpu', 'time_unit', 'policy', "
                       "'task', 'start', 'end') or an edge");
      advance();
      synchronize_top();
      break;
  }
}

void Parser::parse_system_int(std::vector<ast::Spanned<std::int64_t>>& target,
                              const std::string& subject) {
  const Token& keyword = advance();
  if (!check(TokenKind::Integer)) {
    error_unexpected(peek(), "an integer after '" + keyword.text + "'");
    diagnostics_.mark_tainted(subject);
    synchronize_top();
    return;
  }
  const Token& value = advance();
  target.push_back(ast::Spanned<std::int64_t>{value.int_value, span_from(keyword)});
  source_map_.bind(subject, span_from(keyword));
}

void Parser::parse_system_string(std::vector<ast::Spanned<std::string>>& target,
                                 const std::string& subject) {
  const Token& keyword = advance();
  if (!check(TokenKind::Identifier)) {
    error_unexpected(peek(), "an identifier after '" + keyword.text + "'");
    diagnostics_.mark_tainted(subject);
    synchronize_top();
    return;
  }
  const Token& value = advance();
  target.push_back(ast::Spanned<std::string>{value.text, span_from(keyword)});
  source_map_.bind(subject, span_from(keyword));
}

void Parser::parse_task(ast::Program& program) {
  const Token& keyword = advance();
  if (!check(TokenKind::Identifier)) {
    error_unexpected(peek(), "an identifier after 'task'");
    synchronize_top();
    return;
  }

  ast::TaskDecl task;
  const Token& id_token = advance();
  task.id = id_token.text;
  task.id_span = span_of(id_token);
  task.keyword_span = span_of(keyword);
  bool failed = false;

  if (match(TokenKind::Colon)) {
    const std::optional<ast::TaskKind> kind = task_kind_for(peek().kind);
    if (kind.has_value()) {
      task.kind = kind;
      task.kind_span = span_of(advance());
    } else {
      error_unexpected(peek(),
                       "a task kind ('normal', 'periodic', 'aperiodic', 'sporadic', or "
                       "'interrupt')");
      failed = true;
      // Recover the common `task T: bogus {` shape so the block is still read.
      if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::LBrace) {
        advance();
      } else {
        synchronize_top();
      }
    }
  }

  if (!match(TokenKind::LBrace)) {
    error_unexpected(peek(), "'{' to open the task block");
    task.span = span_from(keyword);
    mark_task_tainted(task.id);
    program.tasks.push_back(std::move(task));
    synchronize_top();
    return;
  }

  if (parse_task_attributes(task)) {
    failed = true;
  }

  if (check(TokenKind::RBrace)) {
    const Token& closing = advance();
    task.span = Span{file_, keyword.offset, closing.end_offset(), keyword.line, keyword.column};
  } else {
    error_unexpected(peek(), "'}' to close task '" + task.id + "'");
    task.span = span_from(keyword);
    failed = true;
  }

  if (failed) {
    mark_task_tainted(task.id);
  }
  bind_task_source_map(task);
  program.tasks.push_back(std::move(task));
}

bool Parser::parse_task_attributes(ast::TaskDecl& task) {
  bool failed = false;
  while (!check(TokenKind::EndOfFile) && !check(TokenKind::RBrace)) {
    skip_semicolons();
    if (check(TokenKind::EndOfFile) || check(TokenKind::RBrace)) {
      break;
    }
    if (check(TokenKind::Error)) {
      advance();  // E-SYN-101 already emitted by the lexer.
      failed = true;
      continue;
    }
    switch (peek().kind) {
      case TokenKind::KwPeriod:
        failed = !parse_time_attr(task.period) || failed;
        break;
      case TokenKind::KwDeadline:
        failed = !parse_time_attr(task.deadline) || failed;
        break;
      case TokenKind::KwMit:
        failed = !parse_time_attr(task.mit) || failed;
        break;
      case TokenKind::KwPriority:
        failed = !parse_int_attr(task.priority) || failed;
        break;
      case TokenKind::KwCore:
        failed = !parse_core_attr(task) || failed;
        break;
      case TokenKind::KwLocks:
        failed = !parse_locks_attr(task) || failed;
        break;
      case TokenKind::KwTime:
        failed = !parse_time_decl(task) || failed;
        break;
      default:
        error_unexpected(peek(),
                         "a task attribute ('period', 'deadline', 'mit', 'priority', "
                         "'core', 'locks', 'time') or '}'");
        failed = true;
        advance();
        synchronize_task();
        break;
    }
  }
  return failed;
}

bool Parser::parse_time_attr(std::vector<ast::Spanned<ast::TimeValue>>& target) {
  const Token& keyword = advance();
  if (!check(TokenKind::Integer) && !check(TokenKind::TimeLiteral)) {
    error_unexpected(peek(), "a time value after '" + keyword.text + "'");
    synchronize_task();
    return false;
  }
  const Token& token = advance();
  ast::TimeValue value;
  value.raw_int = token.int_value;
  value.unit = token.unit;
  value.span = span_of(token);
  target.push_back(ast::Spanned<ast::TimeValue>{std::move(value), span_from(keyword)});
  return true;
}

bool Parser::parse_int_attr(std::vector<ast::Spanned<std::int64_t>>& target) {
  const Token& keyword = advance();
  if (!check(TokenKind::Integer)) {
    error_unexpected(peek(), "an integer after '" + keyword.text + "'");
    synchronize_task();
    return false;
  }
  const Token& token = advance();
  target.push_back(ast::Spanned<std::int64_t>{token.int_value, span_from(keyword)});
  return true;
}

bool Parser::parse_core_attr(ast::TaskDecl& task) {
  const Token& keyword = advance();
  ast::CoreSpec spec;
  spec.keyword_span = span_of(keyword);

  if (check(TokenKind::Integer)) {
    spec.kind = ast::CoreSpec::Kind::Single;
    spec.single = advance().int_value;
  } else if (match(TokenKind::KwAny)) {
    spec.kind = ast::CoreSpec::Kind::Any;
  } else if (match(TokenKind::LBrace)) {
    spec.kind = ast::CoreSpec::Kind::Set;
    if (check(TokenKind::Integer)) {
      while (true) {
        const Token& value = advance();
        spec.set.push_back(value.int_value);
        spec.element_spans.push_back(span_of(value));
        if (!match(TokenKind::Comma)) {
          break;
        }
        if (!check(TokenKind::Integer)) {
          error_unexpected(peek(), "an integer core id after ',' in the core set");
          synchronize_task();
          return false;
        }
      }
    } else if (!check(TokenKind::RBrace)) {
      error_unexpected(peek(), "an integer core id or '}'");
      synchronize_task();
      return false;
    }
    if (!match(TokenKind::RBrace)) {
      error_unexpected(peek(), "',' or '}' in the core set");
      synchronize_task();
      return false;
    }
  } else {
    error_unexpected(peek(), "an integer core id, 'any', or '{'");
    synchronize_task();
    return false;
  }

  spec.span = span_from(keyword);
  task.core.push_back(std::move(spec));
  return true;
}

bool Parser::parse_locks_attr(ast::TaskDecl& task) {
  const Token& keyword = advance();
  ast::LocksDecl declaration;
  declaration.keyword_span = span_of(keyword);

  if (!check(TokenKind::Identifier)) {
    error_unexpected(peek(), "an identifier after 'locks'");
    synchronize_task();
    return false;
  }

  bool failed = false;
  while (true) {
    const Token& lock = advance();
    declaration.locks.push_back(ast::IdentRef{lock.text, span_of(lock)});
    if (!match(TokenKind::Comma)) {
      break;
    }
    if (!check(TokenKind::Identifier)) {
      error_unexpected(peek(), "an identifier after ',' in the lock list");
      failed = true;
      break;
    }
  }

  declaration.span = span_from(keyword);
  task.locks.push_back(std::move(declaration));
  if (failed) {
    synchronize_task();
  }
  return !failed;
}

bool Parser::parse_time_decl(ast::TaskDecl& task) {
  const Token& keyword = advance();
  ast::TimeDecl declaration;
  declaration.keyword_span = span_of(keyword);

  bool failed = false;
  if (check(TokenKind::LBracket)) {
    failed = !parse_intervals(declaration);
  }

  if (!failed) {
    failed = !parse_provenance(declaration.provenance);
  }

  declaration.span = span_from(keyword);
  task.time.push_back(std::move(declaration));
  if (failed) {
    synchronize_task();
  }
  return !failed;
}

bool Parser::parse_intervals(ast::TimeDecl& declaration) {
  const Token& opening = advance();  // '['
  if (check(TokenKind::LBracket)) {
    while (true) {
      ast::TimeInterval interval;
      if (!parse_interval(interval)) {
        return false;
      }
      declaration.intervals.push_back(std::move(interval));
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
    if (!match(TokenKind::RBracket)) {
      error_unexpected(peek(), "',' or ']' in the time interval list");
      return false;
    }
  } else {
    ast::TimeInterval interval;
    if (!parse_time_value(interval.min)) {
      return false;
    }
    if (!match(TokenKind::Comma)) {
      error_unexpected(peek(), "',' between the interval bounds");
      return false;
    }
    if (!parse_time_value(interval.max)) {
      return false;
    }
    if (!match(TokenKind::RBracket)) {
      error_unexpected(peek(), "']' after the interval bounds");
      return false;
    }
    interval.span = span_from(opening);
    declaration.intervals.push_back(std::move(interval));
  }
  declaration.has_intervals = true;
  return true;
}

bool Parser::parse_interval(ast::TimeInterval& interval) {
  if (!check(TokenKind::LBracket)) {
    error_unexpected(peek(), "'[' to start a time interval");
    return false;
  }
  const Token& opening = advance();
  if (!parse_time_value(interval.min)) {
    return false;
  }
  if (!match(TokenKind::Comma)) {
    error_unexpected(peek(), "',' between the interval bounds");
    return false;
  }
  if (!parse_time_value(interval.max)) {
    return false;
  }
  if (!match(TokenKind::RBracket)) {
    error_unexpected(peek(), "']' after the interval bounds");
    return false;
  }
  interval.span = span_from(opening);
  return true;
}

bool Parser::parse_time_value(ast::TimeValue& value) {
  if (!check(TokenKind::Integer) && !check(TokenKind::TimeLiteral)) {
    error_unexpected(peek(), "a time value (an integer with an optional 'ms' or 's' suffix)");
    return false;
  }
  const Token& token = advance();
  value.raw_int = token.int_value;
  value.unit = token.unit;
  value.span = span_of(token);
  return true;
}

bool Parser::parse_provenance(ast::Provenance& provenance) {
  switch (peek().kind) {
    case TokenKind::KwEstimated: {
      const Token& keyword = advance();
      provenance.kind = ast::ProvenanceKind::Estimated;
      if (match(TokenKind::KwBy)) {
        if (!check(TokenKind::String)) {
          error_unexpected(peek(), "a string after 'estimated by'");
          return false;
        }
        const Token& note = advance();
        provenance.by = ast::Spanned<std::string>{note.string_value, span_of(note)};
      }
      provenance.span = span_from(keyword);
      return true;
    }
    case TokenKind::KwCertified: {
      const Token& keyword = advance();
      provenance.kind = ast::ProvenanceKind::Certified;
      if (!match(TokenKind::KwBy)) {
        error_unexpected(peek(), "'by' after 'certified'");
        return false;
      }
      if (!check(TokenKind::String)) {
        error_unexpected(peek(), "a string (tool name) after 'certified by'");
        return false;
      }
      const Token& tool = advance();
      provenance.by = ast::Spanned<std::string>{tool.string_value, span_of(tool)};
      if (match(TokenKind::LBrace)) {
        if (!match(TokenKind::KwArtifact)) {
          error_unexpected(peek(), "'artifact' in the certified provenance block");
          return false;
        }
        if (!check(TokenKind::String)) {
          error_unexpected(peek(), "a string after 'artifact'");
          return false;
        }
        const Token& artifact = advance();
        provenance.artifact = ast::Spanned<std::string>{artifact.string_value, span_of(artifact)};
        if (!match(TokenKind::RBrace)) {
          error_unexpected(peek(), "'}' to close the certified provenance block");
          return false;
        }
      }
      provenance.span = span_from(keyword);
      return true;
    }
    case TokenKind::KwMeasured: {
      const Token& keyword = advance();
      provenance.kind = ast::ProvenanceKind::Measured;
      if (!match(TokenKind::KwBy)) {
        error_unexpected(peek(), "'by' after 'measured'");
        return false;
      }
      if (!match(TokenKind::KwLlm)) {
        error_unexpected(peek(), "'llm' after 'measured by'");
        return false;
      }
      if (!check(TokenKind::LBrace)) {
        error_unexpected(peek(), "'{' to open the measurement configuration");
        return false;
      }
      if (!parse_measured_block(provenance.measure)) {
        return false;
      }
      provenance.span = span_from(keyword);
      return true;
    }
    default:
      error_unexpected(peek(), "'estimated', 'certified', or 'measured' after the time value(s)");
      return false;
  }
}

bool Parser::parse_measured_block(ast::MeasureBlock& block) {
  const Token& opening = advance();  // '{'
  if (check(TokenKind::RBrace)) {
    error_unexpected(peek(), "'runs', 'timeout', or 'artifact' in the measurement configuration");
    return false;
  }

  while (true) {
    switch (peek().kind) {
      case TokenKind::KwRuns: {
        const Token& keyword = advance();
        if (!check(TokenKind::Integer)) {
          error_unexpected(peek(), "an integer after 'runs'");
          return false;
        }
        const Token& value = advance();
        block.runs.push_back(ast::Spanned<std::int64_t>{value.int_value, span_from(keyword)});
        break;
      }
      case TokenKind::KwTimeout: {
        const Token& keyword = advance();
        if (!check(TokenKind::Integer)) {
          error_unexpected(peek(), "an integer after 'timeout'");
          return false;
        }
        const Token& value = advance();
        block.timeout.push_back(ast::Spanned<std::int64_t>{value.int_value, span_from(keyword)});
        break;
      }
      case TokenKind::KwArtifact: {
        const Token& keyword = advance();
        if (!check(TokenKind::String)) {
          error_unexpected(peek(), "a string after 'artifact'");
          return false;
        }
        const Token& value = advance();
        block.artifacts.push_back(
            ast::Spanned<std::string>{value.string_value, span_from(keyword)});
        break;
      }
      default:
        error_unexpected(peek(),
                         "'runs', 'timeout', or 'artifact' in the measurement configuration");
        return false;
    }
    if (!match(TokenKind::Comma)) {
      break;
    }
  }

  if (!match(TokenKind::RBrace)) {
    error_unexpected(peek(), "',' or '}' in the measurement configuration");
    return false;
  }
  block.span = span_from(opening);
  return true;
}

void Parser::parse_binding(ast::Program& program, ast::BindingKind kind) {
  const Token& keyword = advance();
  const char* keyword_text = kind == ast::BindingKind::Start ? "start" : "end";
  if (!check(TokenKind::Identifier)) {
    error_unexpected(peek(), "an identifier after '" + std::string(keyword_text) + "'");
    synchronize_top();
    return;
  }
  const Token& task = advance();
  ast::BindingDecl binding;
  binding.kind = kind;
  binding.task = ast::IdentRef{task.text, span_of(task)};
  binding.span = span_from(keyword);
  source_map_.bind(std::string(keyword_text) + ":" + task.text, binding.span);
  program.bindings.push_back(std::move(binding));
}

void Parser::parse_edge(ast::Program& program) {
  const Token& first = peek();
  ast::EdgeDecl edge;
  if (!parse_edge_endpoint(edge.sources, edge.source_braced, "source")) {
    synchronize_top();
    return;
  }
  if (!match(TokenKind::Arrow)) {
    error_unexpected(peek(), "'->' between the edge endpoints");
    synchronize_top();
    return;
  }
  edge.arrow_span = span_of(tokens_[position_ - 1]);
  if (!parse_edge_endpoint(edge.targets, edge.target_braced, "target")) {
    synchronize_top();
    return;
  }
  edge.span = span_from(first);

  const std::vector<std::string> source_names = edge.source_names();
  const std::vector<std::string> target_names = edge.target_names();
  source_map_.bind(ast::canonical_edge_subject(source_names, target_names), edge.span);
  if (source_names.size() == 1 && target_names.size() >= 2) {
    source_map_.bind("fork:" + ast::canonical_fork_name(source_names.front(), target_names),
                     edge.span);
  }
  if (target_names.size() == 1 && source_names.size() >= 2) {
    source_map_.bind("join:" + ast::canonical_join_name(source_names, target_names.front()),
                     edge.span);
  }
  program.edges.push_back(std::move(edge));
}

bool Parser::parse_edge_endpoint(std::vector<ast::IdentRef>& references, bool& braced,
                                 const char* side) {
  references.clear();
  braced = false;

  if (!check(TokenKind::LBrace)) {
    if (check(TokenKind::Identifier)) {
      const Token& identifier = advance();
      references.push_back(ast::IdentRef{identifier.text, span_of(identifier)});
      return true;
    }
    error_unexpected(peek(), "an identifier or '{' for the edge " + std::string(side));
    return false;
  }

  braced = true;
  advance();  // '{'
  if (check(TokenKind::RBrace)) {
    error_unexpected(peek(), "an identifier in the edge " + std::string(side) + " set");
    advance();  // consume the empty set's '}' so recovery is not confused by it
    return false;
  }

  while (true) {
    if (!check(TokenKind::Identifier)) {
      error_unexpected(peek(), "an identifier in the edge " + std::string(side) + " set");
      return false;
    }
    const Token& identifier = advance();
    references.push_back(ast::IdentRef{identifier.text, span_of(identifier)});
    if (!match(TokenKind::Comma)) {
      break;
    }
  }

  if (!match(TokenKind::RBrace)) {
    error_unexpected(peek(), "',' or '}' in the edge " + std::string(side) + " set");
    return false;
  }
  return true;
}

}  // namespace dsl
