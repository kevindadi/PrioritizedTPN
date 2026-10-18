#ifndef TYPES_H
#define TYPES_H

#include <string>
#include <variant>
#include <vector>

enum class TaskType { NORMAL, PERIOD, APERIOD, INTERRUPT };

// 实时系统调度策略
enum class SchedulePolicy {
  FIXED,                     // Fixed Priority - 固定优先级(兼容别名,默认按 resume 处理)
  FIXED_PRIOR_WITH_RESTART,  // Fixed Priority with restart
  FIXED_PRIOR_WITH_RESUME,   // Fixed Priority with resume
  RM,                        // Rate Monotonic - 周期越短优先级越高
  DM,                        // Deadline Monotonic - 截止时间越短优先级越高
  EDF,                       // Earliest Deadline First - 截止时间最早优先
  LLF,                       // Least Laxity First - 松弛时间最小优先
  FIFO,                      // First In First Out - 先来先服务
  PIP,                       // Priority Inheritance Protocol - 优先级继承协议
  PCP,                       // Priority Ceiling Protocol - 优先级天花板协议
  SRP,                       // Stack Resource Policy - 栈资源策略
  UNKNOWN                    // 未知策略
};

SchedulePolicy parse_schedule_policy(const std::string& policy);
std::string schedule_policy_to_string(SchedulePolicy policy);

struct TaskNode {
  std::string name;
  int core = 0;
  int priority = 100;
  std::vector<std::pair<int, int>> time;
  bool is_lock = false;
  std::vector<std::string> lock;
  TaskType task_type = TaskType::NORMAL;
};

// Fork/Join are modelled as PTPN transitions. By default they are zero-time
// control transitions on the control core (-1), but the JSON may override the
// firing interval, the core (to place the sync on a real CPU), and the priority.
struct ForkTask {
  std::string name;
  std::pair<int, int> time = std::make_pair(0, 0);
  int core = -1;
  int priority = 0;
  ForkTask() = default;

  ForkTask(const std::string& n) : name(n) {}
};

struct JoinTask {
  std::string name;
  std::pair<int, int> time = std::make_pair(0, 0);
  int core = -1;
  int priority = 0;
  JoinTask() = default;

  JoinTask(const std::string& n) : name(n) {}
};

struct EmptyTask {
  std::string name;
};

using NodeType = std::variant<TaskNode, ForkTask, JoinTask, EmptyTask>;

struct TaskConfig {
  int core;
  int priority;
  std::vector<std::pair<int, int>> times;
  std::vector<std::string> locks;
};

struct StartBinding {
  std::string task;
  int tokens = 1;
};

struct PeriodicBinding {
  std::string task;
  int period = 0;
};

// Directed edge in a task dependency graph, mirroring the JSON edge object.
struct TdgEdge {
  std::string source;
  std::string target;
  std::string label;
  std::string style;

  [[nodiscard]] bool is_self_loop() const {
    return source == target;
  }

  [[nodiscard]] bool is_dashed() const {
    return style.find("dashed") != std::string::npos;
  }

  [[nodiscard]] bool leaves(const std::string& node) const {
    return source == node && target != node;
  }

  [[nodiscard]] bool enters(const std::string& node) const {
    return target == node && source != node;
  }
};

// Returns a pointer to the embedded TaskNode, or nullptr for non-task nodes.
inline const TaskNode* as_task_node(const NodeType& node) {
  if (std::holds_alternative<TaskNode>(node)) {
    return &std::get<TaskNode>(node);
  }
  return nullptr;
}

inline TaskNode* as_task_node(NodeType& node) {
  if (std::holds_alternative<TaskNode>(node)) {
    return &std::get<TaskNode>(node);
  }
  return nullptr;
}

inline bool is_fork_or_join(const NodeType& node) {
  return std::holds_alternative<ForkTask>(node) || std::holds_alternative<JoinTask>(node);
}

// Applies a visitor to each NodeType alternative (C++17 std::visit wrapper).
template <typename Visitor>
decltype(auto) visit_node(const NodeType& node, Visitor&& visitor) {
  return std::visit(std::forward<Visitor>(visitor), node);
}

#endif  // TYPES_H
