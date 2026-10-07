#ifndef PARSER_PTPN_PARSER_H
#define PARSER_PTPN_PARSER_H

#include <string>
#include <vector>

#include "common/result.h"
#include "petri/petri.h"

namespace parser {

struct PlaceNode {
  std::string id;
  std::string name;
  int capacity = 1;
};

struct TransitionNode {
  std::string id;
  std::string name;
  int time_min = 0;
  int time_max = 0;
  bool left_open = false;
  bool right_open = false;
  int priority = 0;
  int core = -1;
  bool suspendable = false;
};

struct ArcNode {
  std::string source;
  std::string target;
  int weight = 1;
};

struct InitNode {
  std::string place;
  int tokens = 1;
};

struct PTPNAST {
  std::vector<PlaceNode> places;
  std::vector<TransitionNode> transitions;
  std::vector<ArcNode> arcs;
  std::vector<InitNode> initial_marking;
};

class PTPNParser {
 public:
  // Parse PTPN from string (syntax only)
  static bool parse(const std::string& input, PTPNAST& ast, std::string& error);

  // Parse from file (syntax only)
  static bool parse_file(const std::string& filepath, PTPNAST& ast, std::string& error);

  // Semantic validation on a parsed AST
  static bool validate(const PTPNAST& ast, std::string& error);
};

class PTPNBuilder {
 public:
  static petri::PTPN build(const PTPNAST& ast);
  static ptpn::Result<petri::PTPN> parse(const std::string& source);
  static ptpn::Result<petri::PTPN> parse_file(const std::string& filepath);
};

}  // namespace parser

#endif  // PARSER_PTPN_PARSER_H