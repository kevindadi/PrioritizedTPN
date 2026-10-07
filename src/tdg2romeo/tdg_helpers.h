#ifndef TDG2ROMEO_TDG_HELPERS_H
#define TDG2ROMEO_TDG_HELPERS_H

#include <string>
#include <unordered_map>
#include <vector>

#include "romeo_model.h"
#include "tdg/tdg.h"

namespace romeo {

constexpr int kControlPriority = 0;
constexpr int kInfTime = std::numeric_limits<int>::max();

RomeoTimeInterval parse_edge_interval(const std::string& label, const std::string& source_name,
                                      const std::string& target_name);

struct TaskEndpoints {
  std::string entry;
  std::string exit;
  int core = 0;
  int priority = 0;
  std::string active_place;  // inhibitor format: tracks on-core activity
};

struct NodeEndpoints {
  std::string entry;
  std::string exit;
  bool is_transition_node = false;
};

bool has_non_self_successor(const tdg::TDG& tdg, const std::string& task_name);

bool has_self_loop_release(const tdg::TDG& tdg, const std::string& task_name);

std::unordered_map<int, std::vector<std::string>> group_tasks_by_core(const tdg::TDG& tdg);

std::string core_place_name(int core_id);

std::string core_busy_place_name(int core_id);

std::string place_name(const std::string& task, const std::string& suffix);

std::string assignment_expr(const std::string& place, int delta);

std::string guard_ge(const std::string& place, int weight = 1);

std::string guard_and(const std::vector<std::string>& clauses);

}  // namespace romeo

#endif  // TDG2ROMEO_TDG_HELPERS_H
