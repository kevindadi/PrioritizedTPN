#include "parse/ptpn_parser.h"

#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace parser {

static bool is_whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_whitespace_and_comments(std::string::const_iterator& it,
                                         const std::string::const_iterator& end) {
  while (it != end) {
    while (it != end && is_whitespace(*it)) {
      ++it;
    }

    if (it != end && *it == '/' && it + 1 != end && *(it + 1) == '/') {
      while (it != end && *it != '\n') {
        ++it;
      }
      continue;
    }

    if (it != end && *it == '/' && it + 1 != end && *(it + 1) == '*') {
      it += 2;
      while (it != end && !(*it == '*' && it + 1 != end && *(it + 1) == '/')) {
        ++it;
      }
      if (it != end)
        it += 2;
      continue;
    }

    break;
  }
}

static std::string read_identifier(std::string::const_iterator& it,
                                   std::string::const_iterator end) {
  std::string result;
  while (it != end && (std::isalnum(*it) || *it == '_')) {
    result += *it++;
  }
  return result;
}

static int read_number(std::string::const_iterator& it, std::string::const_iterator end) {
  int result = 0;
  while (it != end && std::isdigit(*it)) {
    result = result * 10 + (*it++ - '0');
  }
  return result;
}

static int read_signed_number(std::string::const_iterator& it, std::string::const_iterator end) {
  bool negative = false;
  if (it != end && *it == '-') {
    negative = true;
    ++it;
  } else if (it != end && *it == '+') {
    ++it;
  }
  const int value = read_number(it, end);
  return negative ? -value : value;
}

static bool parse_transition_attribute(TransitionNode& trans, std::string::const_iterator& it,
                                       const std::string::const_iterator& end) {
  skip_whitespace_and_comments(it, end);
  if (it == end || *it != '@') {
    return false;
  }

  ++it;
  skip_whitespace_and_comments(it, end);

  if (it != end && (std::isdigit(*it) || *it == '-')) {
    trans.priority = read_signed_number(it, end);
    return true;
  }

  const std::string attr = read_identifier(it, end);
  if (attr == "priority" || attr == "core" || attr == "capacity") {
    skip_whitespace_and_comments(it, end);
    if (it != end && *it == '=') {
      ++it;
    }
    const int val = read_signed_number(it, end);
    if (attr == "priority") {
      trans.priority = val;
    } else if (attr == "core") {
      trans.core = val;
    }
    return true;
  }

  return false;
}

static bool parse_bare_transition_attribute(TransitionNode& trans, std::string::const_iterator& it,
                                            const std::string::const_iterator& end) {
  if (it == end || !std::isalpha(*it)) {
    return false;
  }

  const auto word_start = it;
  std::string word;
  while (it != end && std::isalpha(*it)) {
    word += *it++;
  }

  if (word == "suspendable") {
    trans.suspendable = true;
    return true;
  }

  if (word == "core" || word == "priority") {
    skip_whitespace_and_comments(it, end);
    if (it != end && *it == '=') {
      ++it;
    }
    const int val = read_signed_number(it, end);
    if (word == "priority") {
      trans.priority = val;
    } else {
      trans.core = val;
    }
    return true;
  }

  it = word_start;
  return false;
}

namespace {

bool validate_impl(const PTPNAST& ast, std::string& error, ptpn::Diagnostics* diagnostics,
                   const std::string& file) {
  std::optional<ptpn::SourceLocation> location;
  if (!file.empty()) {
    ptpn::SourceLocation file_location;
    file_location.file = file;
    location = std::move(file_location);
  }

  const auto fail = [&](const char* code, const std::string& message,
                        std::optional<std::string> subject,
                        std::optional<std::string> fix = std::nullopt) {
    error = message;
    if (diagnostics != nullptr) {
      diagnostics->add(code, message, std::move(subject), location, std::move(fix));
    }
    return false;
  };

  std::set<std::string> place_ids;
  for (const auto& place : ast.places) {
    if (place.id.empty()) {
      return fail("E-VAL-101", "Place definition has empty id", std::nullopt,
                  "give every place a non-empty identifier");
    }
    if (!place_ids.insert(place.id).second) {
      return fail("E-VAL-102", "Duplicate place id: " + place.id, "place:" + place.id,
                  "rename one of the duplicate places");
    }
    if (place.capacity < 0) {
      return fail("E-VAL-103", "Negative capacity for place: " + place.id, "place:" + place.id,
                  "set the place capacity to a non-negative integer");
    }
  }

  std::set<std::string> transition_ids;
  for (const auto& trans : ast.transitions) {
    if (trans.id.empty()) {
      return fail("E-VAL-104", "Transition definition has empty id", std::nullopt,
                  "give every transition a non-empty identifier");
    }
    if (!transition_ids.insert(trans.id).second) {
      return fail("E-VAL-105", "Duplicate transition id: " + trans.id, "transition:" + trans.id,
                  "rename one of the duplicate transitions");
    }
    if (trans.time_min < 0 || trans.time_max < trans.time_min) {
      return fail("E-VAL-106", "Invalid time range for transition: " + trans.id,
                  "transition:" + trans.id,
                  "write the interval as [min, max] with 0 <= min <= max");
    }
    const petri::TimeInterval interval(trans.time_min, trans.time_max, trans.left_open,
                                       trans.right_open);
    if (!interval.is_valid()) {
      return fail("E-VAL-107", "Invalid or empty integer time range for transition: " + trans.id,
                  "transition:" + trans.id,
                  "use a non-empty interval; open bounds require a strict gap");
    }
  }

  std::unordered_map<std::string, bool> node_is_place;
  for (const auto& place : ast.places) {
    node_is_place[place.id] = true;
  }
  for (const auto& trans : ast.transitions) {
    node_is_place[trans.id] = false;
  }

  for (const auto& arc : ast.arcs) {
    const std::string arc_subject = "arc:" + arc.source + "->" + arc.target;
    const auto src_it = node_is_place.find(arc.source);
    const auto tgt_it = node_is_place.find(arc.target);
    if (src_it == node_is_place.end()) {
      return fail("E-VAL-108", "Arc source not found: " + arc.source, arc_subject,
                  "declare the source place or transition before using it in an arc");
    }
    if (tgt_it == node_is_place.end()) {
      return fail("E-VAL-109", "Arc target not found: " + arc.target, arc_subject,
                  "declare the target place or transition before using it in an arc");
    }
    if (src_it->second == tgt_it->second) {
      return fail("E-VAL-110",
                  "Invalid arc (place->place or transition->transition): " + arc.source + " -> " +
                      arc.target,
                  arc_subject,
                  "connect a place to a transition (pre-arc) or a transition to a "
                  "place (post-arc)");
    }
    if (arc.weight <= 0) {
      return fail("E-VAL-111", "Arc weight must be positive: " + arc.source + " -> " + arc.target,
                  arc_subject, "set the arc weight to a positive integer");
    }
  }

  for (const auto& init : ast.initial_marking) {
    if (place_ids.find(init.place) == place_ids.end()) {
      return fail("E-VAL-112", "Initial marking references unknown place: " + init.place,
                  "place:" + init.place, "reference a declared place in @init");
    }
    if (init.tokens < 0) {
      return fail("E-VAL-113", "Negative token count for place: " + init.place,
                  "place:" + init.place, "set the token count to a non-negative integer");
    }
  }

  return true;
}

}  // namespace

bool PTPNParser::validate(const PTPNAST& ast, std::string& error) {
  return validate_impl(ast, error, nullptr, {});
}

bool PTPNParser::validate(const PTPNAST& ast, std::string& error, ptpn::Diagnostics& diagnostics,
                          const std::string& file) {
  return validate_impl(ast, error, &diagnostics, file);
}

namespace {

bool parse_impl(const std::string& input, PTPNAST& ast, std::string& error,
                ptpn::Diagnostics* diagnostics, const std::string& file) {
  ast = PTPNAST();

  auto it = input.begin();
  const auto end = input.end();

  try {
    skip_whitespace_and_comments(it, end);

    while (it != end) {
      skip_whitespace_and_comments(it, end);
      if (it == end)
        break;

      if (*it == '@') {
        ++it;
        const std::string directive = read_identifier(it, end);

        if (directive == "init") {
          skip_whitespace_and_comments(it, end);

          while (it != end && (std::isalpha(*it) || *it == '_')) {
            InitNode init;
            init.place = read_identifier(it, end);
            if (init.place.empty())
              break;

            init.tokens = 1;
            skip_whitespace_and_comments(it, end);

            if (it != end && *it == ':') {
              ++it;
              init.tokens = read_number(it, end);
            }

            ast.initial_marking.push_back(init);
            skip_whitespace_and_comments(it, end);

            if (it != end && *it == ',') {
              ++it;
              skip_whitespace_and_comments(it, end);
            } else {
              break;
            }
          }
        } else if (directive == "capacity") {
          skip_whitespace_and_comments(it, end);

          while (it != end && (std::isalpha(*it) || *it == '_')) {
            const std::string place_id = read_identifier(it, end);
            skip_whitespace_and_comments(it, end);

            if (it != end && *it == ':') {
              ++it;
              const int cap = read_number(it, end);

              for (auto& p : ast.places) {
                if (p.id == place_id) {
                  p.capacity = cap;
                  break;
                }
              }
            }

            skip_whitespace_and_comments(it, end);
            if (it != end && *it == ',') {
              ++it;
              skip_whitespace_and_comments(it, end);
            } else {
              break;
            }
          }
        }
        continue;
      }

      const std::string token = read_identifier(it, end);

      if (token == "places") {
        skip_whitespace_and_comments(it, end);

        while (it != end && *it != '@') {
          if (*it == 't' || *it == 'T') {
            auto lookahead = it;
            const std::string kw = read_identifier(lookahead, end);
            if (kw == "transitions")
              break;
          }

          if (!std::isalpha(*it) && *it != '_') {
            ++it;
            skip_whitespace_and_comments(it, end);
            continue;
          }

          PlaceNode place;
          place.capacity = 1;

          place.id = read_identifier(it, end);
          if (place.id.empty()) {
            ++it;
            continue;
          }

          skip_whitespace_and_comments(it, end);

          if (it != end && *it == ':') {
            ++it;
            skip_whitespace_and_comments(it, end);

            if (it != end && std::isdigit(*it)) {
              place.capacity = read_number(it, end);
            } else {
              place.name = read_identifier(it, end);

              skip_whitespace_and_comments(it, end);

              if (it != end && *it == ':') {
                ++it;
                skip_whitespace_and_comments(it, end);
                place.capacity = read_number(it, end);
              }
            }
          }

          ast.places.push_back(place);
          skip_whitespace_and_comments(it, end);

          if (it != end && *it == ',') {
            ++it;
            skip_whitespace_and_comments(it, end);
          }
        }
      } else if (token == "transitions") {
        skip_whitespace_and_comments(it, end);

        while (it != end && *it != '@') {
          skip_whitespace_and_comments(it, end);

          if (it == end)
            break;

          if (std::isalpha(*it) || *it == '_') {
            auto lookahead_it = it;
            const std::string src = read_identifier(lookahead_it, end);
            skip_whitespace_and_comments(lookahead_it, end);
            if (lookahead_it != end && *lookahead_it == '-' && lookahead_it + 1 != end &&
                *(lookahead_it + 1) == '>') {
              break;
            }
          }

          if (!std::isalpha(*it) && *it != '_' && *it != '[' && *it != '(' && *it != '@') {
            ++it;
            continue;
          }

          TransitionNode trans;
          trans.id = read_identifier(it, end);
          if (trans.id.empty()) {
            ++it;
            continue;
          }

          skip_whitespace_and_comments(it, end);

          if (it != end && *it == ':') {
            ++it;
            skip_whitespace_and_comments(it, end);
            trans.name = read_identifier(it, end);
            skip_whitespace_and_comments(it, end);
          }

          if (it != end && (*it == '[' || *it == '(')) {
            trans.left_open = (*it == '(');
            ++it;
            skip_whitespace_and_comments(it, end);
            trans.time_min = read_number(it, end);
            skip_whitespace_and_comments(it, end);

            if (it != end && *it == ',') {
              ++it;
              skip_whitespace_and_comments(it, end);
              trans.time_max = read_number(it, end);
            }

            skip_whitespace_and_comments(it, end);
            if (it != end && (*it == ']' || *it == ')')) {
              trans.right_open = (*it == ')');
              ++it;
            }
          }

          bool done_with_transition = false;
          while (!done_with_transition && it != end) {
            skip_whitespace_and_comments(it, end);
            if (it == end)
              break;

            if (*it == '@') {
              if (!parse_transition_attribute(trans, it, end)) {
                done_with_transition = true;
                break;
              }
              skip_whitespace_and_comments(it, end);
              if (it != end && *it == ',') {
                ++it;
              }
            } else if (parse_bare_transition_attribute(trans, it, end)) {
              skip_whitespace_and_comments(it, end);
              if (it != end && *it == ',') {
                ++it;
              }
              if (trans.suspendable) {
                break;
              }
            } else if (std::isalpha(*it) || *it == '_') {
              done_with_transition = true;
            } else {
              ++it;
            }
          }

          ast.transitions.push_back(trans);
        }
      } else if (!token.empty()) {
        const std::string src = token;

        skip_whitespace_and_comments(it, end);

        if (it != end && *it == '-' && it + 1 != end && *(it + 1) == '>') {
          it += 2;
          skip_whitespace_and_comments(it, end);

          const std::string tgt = read_identifier(it, end);
          ArcNode arc;
          arc.source = src;
          arc.target = tgt;
          arc.weight = 1;

          skip_whitespace_and_comments(it, end);
          if (it != end && *it == ':') {
            ++it;
            skip_whitespace_and_comments(it, end);
            arc.weight = read_number(it, end);
          }

          ast.arcs.push_back(arc);
        }
      } else {
        ++it;
      }
    }

    return validate_impl(ast, error, diagnostics, file);
  } catch (const std::exception& e) {
    error = e.what();
    if (diagnostics != nullptr) {
      diagnostics->add("E-INT-001", e.what());
    }
    return false;
  }
}

bool parse_file_impl(const std::string& filepath, PTPNAST& ast, std::string& error,
                     ptpn::Diagnostics* diagnostics) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    error = "Cannot open file: " + filepath;
    if (diagnostics != nullptr) {
      ptpn::SourceLocation location;
      location.file = filepath;
      diagnostics->add("E-SYN-002", error, std::nullopt, location,
                       "check that the path exists and is readable");
    }
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return parse_impl(buffer.str(), ast, error, diagnostics, filepath);
}

}  // namespace

bool PTPNParser::parse(const std::string& input, PTPNAST& ast, std::string& error) {
  return parse_impl(input, ast, error, nullptr, {});
}

bool PTPNParser::parse(const std::string& input, PTPNAST& ast, std::string& error,
                       ptpn::Diagnostics& diagnostics) {
  return parse_impl(input, ast, error, &diagnostics, {});
}

bool PTPNParser::parse_file(const std::string& filepath, PTPNAST& ast, std::string& error) {
  return parse_file_impl(filepath, ast, error, nullptr);
}

bool PTPNParser::parse_file(const std::string& filepath, PTPNAST& ast, std::string& error,
                            ptpn::Diagnostics& diagnostics) {
  return parse_file_impl(filepath, ast, error, &diagnostics);
}

petri::PTPN PTPNBuilder::build(const PTPNAST& ast) {
  petri::PTPN ptpn;

  std::unordered_map<std::string, petri::PlaceId> place_index;
  std::unordered_map<std::string, petri::TransitionId> transition_index;

  for (size_t i = 0; i < ast.places.size(); ++i) {
    const auto& p = ast.places[i];
    const std::string name = p.name.empty() ? p.id : p.name;
    place_index[p.id] = ptpn.add_place(name, p.capacity);
  }

  for (size_t i = 0; i < ast.transitions.size(); ++i) {
    const auto& t = ast.transitions[i];
    const std::string name = t.name.empty() ? t.id : t.name;
    const petri::TimeInterval interval(t.time_min, t.time_max, t.left_open, t.right_open);
    transition_index[t.id] = ptpn.add_transition(name, interval, t.priority, t.core, t.suspendable);
  }

  for (const auto& arc : ast.arcs) {
    const auto src_place = place_index.find(arc.source);
    const auto src_trans = transition_index.find(arc.source);
    const auto tgt_place = place_index.find(arc.target);
    const auto tgt_trans = transition_index.find(arc.target);

    if (src_place != place_index.end() && tgt_trans != transition_index.end()) {
      ptpn.set_pre_arc(src_place->second, tgt_trans->second, arc.weight);
    } else if (src_trans != transition_index.end() && tgt_place != place_index.end()) {
      ptpn.set_post_arc(src_trans->second, tgt_place->second, arc.weight);
    }
  }

  petri::Marking marking(ptpn.num_places(), 0);
  for (const auto& init : ast.initial_marking) {
    const auto idx = place_index.find(init.place);
    if (idx != place_index.end()) {
      marking[idx->second.index()] = init.tokens;
    }
  }
  ptpn.set_initial_marking(marking);

  return ptpn;
}

ptpn::Result<petri::PTPN> PTPNBuilder::parse(const std::string& source) {
  PTPNAST ast;
  std::string error;
  if (!PTPNParser::parse(source, ast, error)) {
    return ptpn::Result<petri::PTPN>::failure(std::move(error));
  }
  return ptpn::Result<petri::PTPN>::success(build(ast));
}

ptpn::Result<petri::PTPN> PTPNBuilder::parse(const std::string& source,
                                             ptpn::Diagnostics& diagnostics) {
  PTPNAST ast;
  std::string error;
  if (!PTPNParser::parse(source, ast, error, diagnostics)) {
    return ptpn::Result<petri::PTPN>::failure(std::move(error));
  }
  return ptpn::Result<petri::PTPN>::success(build(ast));
}

ptpn::Result<petri::PTPN> PTPNBuilder::parse_file(const std::string& filepath) {
  PTPNAST ast;
  std::string error;
  if (!PTPNParser::parse_file(filepath, ast, error)) {
    return ptpn::Result<petri::PTPN>::failure(std::move(error));
  }
  return ptpn::Result<petri::PTPN>::success(build(ast));
}

ptpn::Result<petri::PTPN> PTPNBuilder::parse_file(const std::string& filepath,
                                                  ptpn::Diagnostics& diagnostics) {
  PTPNAST ast;
  std::string error;
  if (!PTPNParser::parse_file(filepath, ast, error, diagnostics)) {
    return ptpn::Result<petri::PTPN>::failure(std::move(error));
  }
  return ptpn::Result<petri::PTPN>::success(build(ast));
}

}  // namespace parser
