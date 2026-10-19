#ifndef DSL_SOURCE_MAP_H
#define DSL_SOURCE_MAP_H

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "common/diagnostics.h"

namespace dsl {

// Byte-accurate source span. `byte_end` is exclusive. The span covers
// `[byte_start, byte_end)` in the original file so that a later phase can
// replace the whole range with formatted text.
struct Span {
  std::string file;
  std::size_t byte_start = 0;
  std::size_t byte_end = 0;
  int line = 0;
  int column = 0;

  [[nodiscard]] std::size_t length() const {
    return byte_end >= byte_start ? byte_end - byte_start : 0;
  }

  [[nodiscard]] bool valid() const {
    return line > 0;
  }
};

// Side table from structured subject keys to source spans. It never pollutes
// model::TDG; later stages backfill Diagnostic::location by looking the subject
// up here. A missing key simply leaves the location unset, which the
// diagnostics protocol allows.
class SourceMap {
 public:
  // First binding wins: duplicate declarations are validation errors, and the
  // first occurrence is the stable anchor.
  void bind(const std::string& key, Span span) {
    entries_.emplace(key, std::move(span));
  }

  [[nodiscard]] bool contains(const std::string& key) const {
    return entries_.count(key) != 0;
  }

  [[nodiscard]] const Span* find(const std::string& key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] std::optional<Span> span_for(const std::string& key) const {
    const Span* span = find(key);
    if (span == nullptr) {
      return std::nullopt;
    }
    return *span;
  }

  [[nodiscard]] std::optional<ptpn::SourceLocation> location_for(const std::string& key) const {
    const Span* span = find(key);
    if (span == nullptr) {
      return std::nullopt;
    }
    ptpn::SourceLocation location;
    location.file = span->file;
    location.line = span->line;
    location.column = span->column;
    location.span = static_cast<int>(span->length());
    return location;
  }

  [[nodiscard]] const std::map<std::string, Span>& entries() const {
    return entries_;
  }

  void clear() {
    entries_.clear();
  }

 private:
  std::map<std::string, Span> entries_;
};

}  // namespace dsl

#endif  // DSL_SOURCE_MAP_H
