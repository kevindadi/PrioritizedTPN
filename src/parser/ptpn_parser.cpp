#include "parser/ptpn_parser.h"

#include <fstream>
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

bool PTPNParser::validate(const PTPNAST& ast, std::string& error) {
  std::set<std::string> place_ids;
  for (const auto& place : ast.places) {
    if (place.id.empty()) {
      error = "Place definition has empty id";
      return false;
    }
    if (!place_ids.insert(place.id).second) {
      error = "Duplicate place id: " + place.id;
      return false;
    }
    if (place.capacity < 0) {
      error = "Negative capacity for place: " + place.id;
      return false;
    }
  }

  std::set<std::string> transition_ids;
  for (const auto& trans : ast.transitions) {
    if (trans.id.empty()) {
      error = "Transition definition has empty id";
      return false;
    }
    if (!transition_ids.insert(trans.id).second) {
      error = "Duplicate transition id: " + trans.id;
      return false;
    }
    if (trans.time_min < 0 || trans.time_max < trans.time_min) {
      error = "Invalid time range for transition: " + trans.id;
      return false;
    }
    const petri::TimeInterval interval(trans.time_min, trans.time_max, trans.left_open,
                                       trans.right_open);
    if (!interval.is_valid()) {
      error = "Invalid or empty integer time range for transition: " + trans.id;
      return false;
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
    const auto src_it = node_is_place.find(arc.source);
    const auto tgt_it = node_is_place.find(arc.target);
    if (src_it == node_is_place.end()) {
      error = "Arc source not found: " + arc.source;
      return false;
    }
    if (tgt_it == node_is_place.end()) {
      error = "Arc target not found: " + arc.target;
      return false;
    }
    if (src_it->second == tgt_it->second) {
      error = "Invalid arc (place->place or transition->transition): " + arc.source + " -> " +
              arc.target;
      return false;
    }
    if (arc.weight <= 0) {
      error = "Arc weight must be positive: " + arc.source + " -> " + arc.target;
      return false;
    }
  }

  for (const auto& init : ast.initial_marking) {
    if (place_ids.find(init.place) == place_ids.end()) {
      error = "Initial marking references unknown place: " + init.place;
      return false;
    }
    if (init.tokens < 0) {
      error = "Negative token count for place: " + init.place;
      return false;
    }
  }

  return true;
}

bool PTPNParser::parse(const std::string& input, PTPNAST& ast, std::string& error) {
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

    return validate(ast, error);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool PTPNParser::parse_file(const std::string& filepath, PTPNAST& ast, std::string& error) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    error = "Cannot open file: " + filepath;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return parse(buffer.str(), ast, error);
}

petri::PTPN PTPNBuilder::build(const PTPNAST& ast) {
  petri::PTPN ptpn;

  std::unordered_map<std::string, size_t> place_index;
  std::unordered_map<std::string, size_t> transition_index;

  for (size_t i = 0; i < ast.places.size(); ++i) {
    const auto& p = ast.places[i];
    place_index[p.id] = i;
    const std::string name = p.name.empty() ? p.id : p.name;
    ptpn.add_place(name, p.capacity);
  }

  for (size_t i = 0; i < ast.transitions.size(); ++i) {
    const auto& t = ast.transitions[i];
    transition_index[t.id] = i;
    const std::string name = t.name.empty() ? t.id : t.name;
    const petri::TimeInterval interval(t.time_min, t.time_max, t.left_open, t.right_open);
    ptpn.add_transition(name, interval, t.priority, t.core, t.suspendable);
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
      marking[idx->second] = init.tokens;
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

ptpn::Result<petri::PTPN> PTPNBuilder::parse_file(const std::string& filepath) {
  PTPNAST ast;
  std::string error;
  if (!PTPNParser::parse_file(filepath, ast, error)) {
    return ptpn::Result<petri::PTPN>::failure(std::move(error));
  }
  return ptpn::Result<petri::PTPN>::success(build(ast));
}

}  // namespace parser
