#include "tdg2pn.h"

#include <algorithm>
#include <spdlog/spdlog.h>
#include <sstream>

namespace converter {

namespace {

constexpr int kControlTransitionPriority = 0;
constexpr int kControlTransitionCore = -1;

// Indices into the per-task place/transition chain stored in node_pn_map.
struct TaskChainLayout {
  static constexpr size_t kEntry = 0;
  static constexpr size_t kGetCore = 1;
  static constexpr size_t kReady = 2;
  static constexpr size_t kFirstExec = 3;
  static constexpr size_t kFirstSegDone = 4;
  static constexpr size_t kMinLength = 5;

  static size_t lock_acquire_transition(size_t lock_index) {
    return 5 + 4 * lock_index;
  }

  static size_t lock_release_transition(size_t chain_length, size_t lock_index) {
    return chain_length - 4 - 2 * lock_index;
  }
};

int encode_task_execution_priority(int task_priority) {
  return task_priority;
}

// The resume policy (and the legacy "fixed" alias) no longer relies on the
// structural CPU-resource place or the preempt/suspended/resume sub-net.
// Preemption is expressed natively by the analysis engine (per-core priority
// filtering + execution-clock freeze), so its execution segments are marked
// suspendable. The restart policy keeps the structural encoding untouched.
bool is_resume_policy(SchedulePolicy policy) {
  return policy == SchedulePolicy::FIXED || policy == SchedulePolicy::FIXED_PRIOR_WITH_RESUME;
}

petri::TimeInterval immediate_interval() {
  return petri::TimeInterval(0, 0);
}

// Trims ASCII whitespace from both ends of a string.
std::string trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// Parses a single time bound. Accepts a non-negative integer, or an unbounded
// marker (inf / +inf / ∞ / *) mapped to petri::INF. Returns false on failure.
bool parse_time_bound(const std::string& token, int& out) {
  const std::string value = trim(token);
  if (value.empty()) {
    return false;
  }
  if (value == "inf" || value == "+inf" || value == "∞" || value == "*") {
    out = petri::INF;
    return true;
  }
  try {
    size_t consumed = 0;
    const long parsed = std::stol(value, &consumed);
    if (consumed != value.size() || parsed < 0) {
      return false;
    }
    out = static_cast<int>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// Converts a TDG edge label into the firing interval of the bridge transition
// it induces. Supported forms (surrounding []/() brackets are tolerated):
//   ""        -> [0, 0]   (immediate)
//   "a"       -> [a, a]
//   "a,b"     -> [a, b]   (b may be an unbounded marker)
// Any malformed label falls back to [0, 0] with a warning.
petri::TimeInterval parse_edge_interval(const std::string& label, const std::string& source_name,
                                        const std::string& target_name) {
  std::string body = trim(label);
  if (!body.empty() && (body.front() == '[' || body.front() == '(') &&
      (body.back() == ']' || body.back() == ')')) {
    body = trim(body.substr(1, body.size() - 2));
  }
  if (body.empty()) {
    return immediate_interval();
  }

  const auto comma = body.find(',');
  int earliest = 0;
  int latest = 0;
  bool ok = false;
  if (comma == std::string::npos) {
    ok = parse_time_bound(body, earliest);
    latest = earliest;
  } else {
    ok = parse_time_bound(body.substr(0, comma), earliest) &&
         parse_time_bound(body.substr(comma + 1), latest);
  }

  if (!ok || earliest == petri::INF || (latest != petri::INF && latest < earliest)) {
    spdlog::warn("[TDG2PN] Invalid time label '{}' on edge {} -> {}; using [0, 0]", label,
                 source_name, target_name);
    return immediate_interval();
  }
  return petri::TimeInterval(earliest, latest);
}

size_t add_control_transition(petri::PTPN& ptpn, const std::string& name,
                              const petri::TimeInterval& interval = petri::TimeInterval(0, 0)) {
  return ptpn.add_transition(name, interval, kControlTransitionPriority, kControlTransitionCore,
                             /*suspendable=*/false);
}

std::string format_core_priority_order(int core_id, const std::vector<std::string>& tasks,
                                       const std::unordered_map<std::string, int>& tasks_priority,
                                       const std::string& prefix) {
  std::ostringstream oss;
  oss << prefix << " Core " << core_id << " priority order: ";

  bool first = true;
  for (const auto& task : tasks) {
    if (!first) {
      oss << " > ";
    }
    first = false;
    oss << task << "(" << tasks_priority.at(task) << ")";
  }

  if (first) {
    oss << "(none)";
  }

  return oss.str();
}

}  // namespace

bool TDG2PN::has_non_self_successor(const tdg::TDG& tdg, const std::string& task_name) {
  return std::any_of(tdg.tdg_edges.begin(), tdg.tdg_edges.end(),
                     [&](const TdgEdge& edge) { return edge.leaves(task_name); });
}

bool TDG2PN::has_self_loop_release(const tdg::TDG& tdg, const std::string& task_name) {
  return std::any_of(tdg.tdg_edges.begin(), tdg.tdg_edges.end(), [&](const TdgEdge& edge) {
    return edge.source == task_name && edge.is_self_loop();
  });
}

void TDG2PN::add_consume_transition(petri::PTPN& ptpn, const std::string& task_name,
                                    size_t end_idx) {
  const size_t consume_trans = add_control_transition(ptpn, task_name + "_consume");
  ptpn.set_pre_arc(end_idx, consume_trans, 1);
}

void TDG2PN::add_start_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  for (const auto& start_binding : tdg.start_tasks) {
    const auto node_it = ptpn.node_start_end_map.find(start_binding.task);
    if (node_it == ptpn.node_start_end_map.end()) {
      spdlog::warn("[TDG2PN] Start task not found in node map: {}", start_binding.task);
      continue;
    }
    if (start_binding.tokens <= 0) {
      continue;
    }
    ptpn.set_initial_marking(node_it->second.first, start_binding.tokens);
  }
}

void TDG2PN::add_end_consumers(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  std::set<std::string> consume_tasks;

  for (const auto& [vertex_name, node_type] : tdg.nodes_type) {
    if (as_task_node(node_type) && !has_non_self_successor(tdg, vertex_name)) {
      consume_tasks.insert(vertex_name);
    }
  }

  consume_tasks.insert(tdg.end_tasks.begin(), tdg.end_tasks.end());

  for (const auto& task_name : consume_tasks) {
    const auto node_it = ptpn.node_start_end_map.find(task_name);
    if (node_it == ptpn.node_start_end_map.end()) {
      spdlog::warn("[TDG2PN] End task not found in node map: {}", task_name);
      continue;
    }
    add_consume_transition(ptpn, task_name, node_it->second.second);
  }
}

void TDG2PN::add_periodic_release_bindings(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  for (const auto& periodic_task : tdg.periodic_tasks) {
    if (has_self_loop_release(tdg, periodic_task.task)) {
      continue;
    }

    const auto node_it = ptpn.node_start_end_map.find(periodic_task.task);
    const auto type_it = tdg.nodes_type.find(periodic_task.task);
    if (node_it == ptpn.node_start_end_map.end() || type_it == tdg.nodes_type.end()) {
      spdlog::warn("[TDG2PN] Periodic task not found for release binding: {}", periodic_task.task);
      continue;
    }

    if (!as_task_node(type_it->second)) {
      spdlog::warn("[TDG2PN] Periodic release requested for non-task node: {}", periodic_task.task);
      continue;
    }

    const size_t period_place =
        ptpn.add_place(periodic_task.task + "_period", 1, /*saturate=*/true);
    const size_t fire =
        add_control_transition(ptpn, periodic_task.task + "_fire",
                               petri::TimeInterval(periodic_task.period, periodic_task.period));

    ptpn.set_initial_marking(period_place, 1);
    ptpn.set_pre_arc(period_place, fire, 1);
    ptpn.set_post_arc(fire, period_place, 1);
    ptpn.set_post_arc(fire, node_it->second.first, 1);
  }
}

// ===========================================================================
//  TDG  ->  PTPN   lowering pipeline
// ===========================================================================
//
//  transform() turns a Task Dependency Graph into a Priority Timed Petri Net
//  in five ordered stages:
//
//    TDG ─▶ [1] vertices ─▶ [2] edges ─▶ [3] bindings ─▶ [4] scheduling ─▶ [5]
//    resources
//
//    [1] transform_vertices    every TDG node -> a place/transition fragment
//    [2] transform_edges       TDG edges wire the fragments together
//    [3] start/periodic/end    initial tokens, periodic releases, sink
//    consumers [4] preemption model      resume: engine-level filter (no
//    sub-net);
//                              restart: structural preempt/resume arcs
//    [5] resources + metadata  core/lock resource places, then task_info
//
// ---------------------------------------------------------------------------
//  [1] Node lowering  (add_node_matrix)
// ---------------------------------------------------------------------------
//
//    task node -> a place/transition "chain" (add_execution_chain):
//
//        (entry) ──▶ [get_core] ──▶ (ready) ──▶ [exec] ──▶ (exit)
//         place       T [0,0]        place      T [a,b]     place
//
//      With N locks the single exec splits into 2N+1 timed segments; each lock
//      inserts an immediate acquire transition plus a hold place:
//
//        (ready) ─▶[exec_1]─▶(seg_done)─▶[lock_1]─▶(hold_1)─▶[exec_2]─▶ ...
//        ─▶(exit)
//
//    fork / join -> a single transition (default [0,0], core -1, prio 0; the
//                   interval, core and priority are overridable from JSON)
//    empty       -> a single place
//
//    node_start_end_map[name] = (start, end) records each fragment's entry/exit
//    for stage [2]; node_pn_map[name] keeps the full task chain for resources.
//
// ---------------------------------------------------------------------------
//  [2] Edge lowering  (handle_normal_edge_matrix)
// ---------------------------------------------------------------------------
//
//    task ─▶ task        a bridge transition whose interval is parsed from the
//                        edge label:
//
//        (src.exit) ──▶ [src_to_tgt  <label>] ──▶ (tgt.entry)
//
//    task ─▶ fork/join   (src.exit place) ──▶ [fork/join transition]   label
//    ignored fork/join ─▶ task   [fork/join transition] ──▶ (tgt.entry place)
//    label ignored self-loop           label = period -> deadline monitor
//    sub-net dashed style        periodic release binding (no direct arc added)
//
// ===========================================================================
void TDG2PN::transform(const tdg::TDG& tdg, petri::PTPN& ptpn) {
  try {
    spdlog::info("[TDG2PN] Starting TDG to PTPN transformation");

    ptpn.node_start_end_map.clear();
    ptpn.node_pn_map.clear();
    ptpn.cpus_place.clear();
    ptpn.core_parallelism.clear();
    ptpn.locks_place.clear();
    ptpn.task_info.clear();
    ptpn.node_index = 0;

    std::unordered_map<std::string, TaskConfig> tasks_config;
    for (const auto& node : tdg.all_task) {
      if (const auto* task = as_task_node(node)) {
        tasks_config.emplace(task->name,
                             TaskConfig{task->core, task->priority, task->time, task->lock});
      }
    }

    spdlog::info("[TDG2PN] Transforming vertices");
    transform_vertices(ptpn, tdg);

    spdlog::info("[TDG2PN] Transforming edges");
    transform_edges(ptpn, tdg);

    add_start_bindings(ptpn, tdg);
    add_periodic_release_bindings(ptpn, tdg);
    add_end_consumers(ptpn, tdg);

    if (tdg.policy == SchedulePolicy::FIXED ||
        tdg.policy == SchedulePolicy::FIXED_PRIOR_WITH_RESUME) {
      spdlog::info(
          "[TDG2PN] Resume preemption is expressed by the analysis engine "
          "(per-core priority filter + execution-clock freeze); no structural "
          "preempt/resume sub-net is generated");
    } else if (tdg.policy == SchedulePolicy::FIXED_PRIOR_WITH_RESTART) {
      spdlog::info("[TDG2PN] Creating fixed-priority restart preemption relations");
      const auto core_task = classify_tdg_priority(tdg);
      fixed_prior_with_restart(ptpn, core_task, tasks_config, tdg.nodes_type);
    } else {
      spdlog::info("[TDG2PN] Skipping preemption expansion for non-fixed policy");
    }

    spdlog::info("[TDG2PN] Adding resources and bindings");
    add_resources_and_bindings_matrix(ptpn, tdg);

    populate_task_info(ptpn, tdg);

    spdlog::info("[TDG2PN] TDG transformation completed: {} places, {} transitions",
                 ptpn.places.size(), ptpn.transitions.size());

    if (!ptpn.verify_structure()) {
      spdlog::warn("[TDG2PN] Structure verification failed, continuing anyway");
    }
  } catch (const std::exception& e) {
    spdlog::error("[TDG2PN] Failed to transform TDG to PTPN: {}", e.what());
    throw;
  }
}

void TDG2PN::populate_task_info(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  // Period from explicit periodic bindings, then from self-loop edge labels
  // (the deadline monitor uses the same value).
  std::unordered_map<std::string, int> period_of;
  for (const auto& binding : tdg.periodic_tasks) {
    period_of[binding.task] = binding.period;
  }
  for (const auto& edge : tdg.tdg_edges) {
    if (!edge.is_self_loop() || period_of.count(edge.source)) {
      continue;
    }
    try {
      period_of[edge.source] = std::stoi(edge.label);
    } catch (const std::exception&) {
      // Non-numeric self-loop label: leave aperiodic.
    }
  }

  for (const auto& node : tdg.all_task) {
    const auto* task = as_task_node(node);
    if (task == nullptr) {
      continue;
    }
    petri::TaskInfo info;
    info.core = task->core;
    info.priority = task->priority;
    info.wcet = 0;
    info.bcet = 0;
    for (const auto& segment : task->time) {
      info.bcet += segment.first;
      info.wcet += segment.second;
    }
    const auto period_it = period_of.find(task->name);
    info.period = period_it == period_of.end() ? 0 : period_it->second;
    info.deadline = info.period;  // implicit deadline = period
    const auto locks_it = tdg.task_locks_map.find(task->name);
    if (locks_it != tdg.task_locks_map.end()) {
      info.locks = locks_it->second;
    } else {
      info.locks = task->lock;
    }
    ptpn.task_info[task->name] = std::move(info);
  }
}

std::unordered_map<int, std::vector<std::string>> TDG2PN::classify_tdg_priority(
    const tdg::TDG& tdg) {
  std::unordered_map<int, std::vector<std::string>> core_task;

  for (const auto& node : tdg.all_task) {
    if (const auto* task = as_task_node(node)) {
      core_task[task->core].push_back(task->name);
    }
  }

  for (auto& [core_id, tasks] : core_task) {
    std::sort(tasks.begin(), tasks.end(), [&](const std::string& left, const std::string& right) {
      return tdg.tasks_priority.at(left) > tdg.tasks_priority.at(right);
    });
    spdlog::info("{}", format_core_priority_order(core_id, tasks, tdg.tasks_priority, "[TDG2PN]"));
  }

  return core_task;
}

std::unordered_map<std::string, int> TDG2PN::build_preempt_priorities(
    const std::vector<std::string>& tasks, const std::unordered_map<std::string, TaskConfig>& tc,
    int aggressor_priority) {
  std::unordered_map<std::string, int> priorities;

  for (const auto& task_name : tasks) {
    const auto task_it = tc.find(task_name);
    if (task_it == tc.end()) {
      continue;
    }

    if (task_it->second.priority >= aggressor_priority) {
      continue;
    }

    priorities[task_name] = aggressor_priority;
  }

  return priorities;
}

void TDG2PN::transform_vertices(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  const bool resume_mode = is_resume_policy(tdg.policy);
  for (const auto& node_entry : tdg.nodes_type) {
    const std::string& vertex_name = node_entry.first;
    const NodeType& node_type = node_entry.second;
    spdlog::debug("[TDG2PN] Processing vertex: {}", vertex_name);

    try {
      const auto [start_idx, end_idx] =
          add_node_matrix(ptpn, node_type, resume_mode, tdg.task_place_capacity);
      ptpn.node_start_end_map[vertex_name] = {start_idx, end_idx};

      const bool is_leaf =
          !std::any_of(tdg.tdg_edges.begin(), tdg.tdg_edges.end(),
                       [&](const TdgEdge& edge) { return edge.leaves(vertex_name); });

      if (is_leaf && as_task_node(node_type)) {
        spdlog::debug("[TDG2PN] Leaf task will get consume transition later: {}", vertex_name);
      }
    } catch (const std::exception& e) {
      spdlog::error("[TDG2PN] Failed to transform vertex {}: {}", vertex_name, e.what());
      throw;
    }
  }

  spdlog::info("[TDG2PN] Vertex transformation completed");
}

void TDG2PN::transform_edges(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  for (const auto& edge : tdg.tdg_edges) {
    try {
      if (edge.is_self_loop()) {
        handle_self_loop_edge_matrix(ptpn, edge.label, edge.source);
        continue;
      }

      if (edge.is_dashed()) {
        handle_dashed_edge_matrix(ptpn, edge.source, edge.target);
        continue;
      }

      handle_normal_edge_matrix(ptpn, tdg, edge.source, edge.target, edge.label);
    } catch (const std::exception& exception) {
      spdlog::error("[TDG2PN] Failed to transform edge: {}", exception.what());
      throw;
    }
  }
}

void TDG2PN::handle_self_loop_edge_matrix(petri::PTPN& ptpn, const std::string& label,
                                          const std::string& source_name) {
  int task_period_time = std::stoi(label);
  auto task_start_end = ptpn.node_start_end_map.find(source_name);
  if (task_start_end == ptpn.node_start_end_map.end()) {
    throw std::runtime_error("Start/end nodes not found for: " + source_name);
  }

  add_monitor_matrix(ptpn, source_name, task_period_time, task_start_end->second.first,
                     task_start_end->second.second);
}

void TDG2PN::handle_dashed_edge_matrix(petri::PTPN& ptpn, const std::string& source_name,
                                       const std::string& target_name) {
  // Dashed edges are handled by periodic release bindings; no direct arc is
  // added.
  (void)ptpn;
  (void)source_name;
  (void)target_name;
}

void TDG2PN::handle_normal_edge_matrix(petri::PTPN& ptpn, const tdg::TDG& tdg,
                                       const std::string& source_name,
                                       const std::string& target_name, const std::string& label) {
  const auto source_it = ptpn.node_start_end_map.find(source_name);
  const auto target_it = ptpn.node_start_end_map.find(target_name);

  if (source_it == ptpn.node_start_end_map.end() || target_it == ptpn.node_start_end_map.end()) {
    throw std::runtime_error("Node mapping not found for edge: " + source_name + " -> " +
                             target_name);
  }

  const auto source_type_it = tdg.nodes_type.find(source_name);
  const auto target_type_it = tdg.nodes_type.find(target_name);
  if (source_type_it == tdg.nodes_type.end() || target_type_it == tdg.nodes_type.end()) {
    throw std::runtime_error("Node type not found for edge: " + source_name + " -> " + target_name);
  }

  const bool source_is_control = is_fork_or_join(source_type_it->second);
  const bool target_is_control = is_fork_or_join(target_type_it->second);

  const size_t source_exit = source_it->second.second;
  const size_t target_entry = target_it->second.first;

  if (source_is_control && target_is_control) {
    throw std::runtime_error("Invalid TDG edge between transition nodes: " + source_name + " -> " +
                             target_name);
  }

  if (source_is_control) {
    if (source_exit < ptpn.transitions.size() && target_entry < ptpn.places.size()) {
      ptpn.set_post_arc(source_exit, target_entry, 1);
    } else {
      throw std::runtime_error("Invalid fork/join to task edge mapping: " + source_name + " -> " +
                               target_name);
    }
    return;
  }

  if (target_is_control) {
    if (source_exit < ptpn.places.size() && target_entry < ptpn.transitions.size()) {
      ptpn.set_pre_arc(source_exit, target_entry, 1);
    } else {
      throw std::runtime_error("Invalid task to fork/join edge mapping: " + source_name + " -> " +
                               target_name);
    }
    return;
  }

  // A task -> task edge is realised by a dedicated bridge transition, whose
  // firing interval comes from the edge label (fork/join-adjacent edges wire
  // directly into the fork/join transition and ignore the label).
  const petri::TimeInterval interval = parse_edge_interval(label, source_name, target_name);
  const size_t bridge_transition =
      add_control_transition(ptpn, source_name + "_to_" + target_name, interval);

  if (source_exit < ptpn.places.size() && target_entry < ptpn.places.size()) {
    ptpn.set_pre_arc(source_exit, bridge_transition, 1);
    ptpn.set_post_arc(bridge_transition, target_entry, 1);
  } else {
    spdlog::warn("[TDG2PN] Unexpected node types for edge: {} -> {}", source_name, target_name);
  }

  spdlog::debug("[TDG2PN] Added edge: {} -> {}", source_name, target_name);
}

void TDG2PN::add_resources_and_bindings_matrix(petri::PTPN& ptpn, const tdg::TDG& tdg) {
  // The resume policy expresses per-core mutual exclusion and preemption purely
  // through the analysis engine's per-core priority filter, so it omits the
  // structural CPU-resource place. Instead it records each CPU's parallelism
  // (cores_per_cpu) so the filter keeps at most that many tasks active per CPU.
  // Other policies (restart / PToPNer export) keep the place-based encoding.
  if (!is_resume_policy(tdg.policy)) {
    add_cpu_resource_matrix(ptpn, tdg.num_cpus, tdg.cores_per_cpu);
  } else {
    // One task per core, matching the physical model (cores_per_cpu parallelism
    // is intentionally not used here).
    for (int cpu = 0; cpu < tdg.num_cpus; ++cpu) {
      ptpn.core_parallelism[cpu] = 1;
    }
  }
  add_lock_resource_matrix(ptpn, tdg.lock_set);
  if (!is_resume_policy(tdg.policy)) {
    task_bind_cpu_resource_matrix(ptpn, tdg.all_task);
  }
  task_bind_lock_resource_matrix(ptpn, tdg.all_task, tdg.task_locks_map);
}

void TDG2PN::add_cpu_resource_matrix(petri::PTPN& ptpn, int cpus, int cores_per_cpu) {
  for (int core = 0; core < cpus; ++core) {
    const std::string core_name = "core" + std::to_string(core);
    const size_t core_place = ptpn.add_place(core_name, cores_per_cpu);
    ptpn.cpus_place.push_back(core_place);
    ptpn.set_initial_marking(core_place, cores_per_cpu);
  }
  spdlog::info("[TDG2PN] Created core resources");
}

void TDG2PN::add_lock_resource_matrix(petri::PTPN& ptpn, const std::set<std::string>& locks_name) {
  if (locks_name.empty()) {
    spdlog::info("[TDG2PN] TDG without locks");
    return;
  }

  for (const auto& lock_name : locks_name) {
    const size_t lock_place = ptpn.add_place(lock_name, 1);
    ptpn.locks_place.emplace(lock_name, lock_place);
    ptpn.set_initial_marking(lock_place, 1);
  }
  spdlog::info("[TDG2PN] Created lock resources");
}

void TDG2PN::task_bind_cpu_resource_matrix(petri::PTPN& ptpn,
                                           const std::vector<NodeType>& all_task) {
  for (const auto& node : all_task) {
    const auto* task = as_task_node(node);
    if (!task) {
      continue;
    }

    const auto chain_it = ptpn.node_pn_map.find(task->name);
    if (chain_it == ptpn.node_pn_map.end()) {
      continue;
    }

    const auto& chain = chain_it->second;
    if (chain.size() < TaskChainLayout::kMinLength) {
      continue;
    }

    ptpn.set_pre_arc(ptpn.cpus_place[task->core], chain[TaskChainLayout::kGetCore], 1);
    ptpn.set_post_arc(chain[chain.size() - 2], ptpn.cpus_place[task->core], 1);
  }
}

void TDG2PN::task_bind_lock_resource_matrix(
    petri::PTPN& ptpn, const std::vector<NodeType>& all_task,
    const std::map<std::string, std::vector<std::string>>& task_locks) {
  if (task_locks.empty()) {
    spdlog::info("[TDG2PN] No task locks to bind");
    return;
  }

  for (const auto& node : all_task) {
    const auto* task = as_task_node(node);
    if (!task) {
      continue;
    }

    const auto chain_it = ptpn.node_pn_map.find(task->name);
    if (chain_it == ptpn.node_pn_map.end()) {
      continue;
    }

    bind_task_locks_matrix(ptpn, task->name, task->lock, chain_it->second, task_locks);
  }

  spdlog::info("[TDG2PN] Completed lock resource binding for all tasks");
}

void TDG2PN::bind_task_locks_matrix(
    petri::PTPN& ptpn, const std::string& task_name, const std::vector<std::string>& lock_types,
    const std::vector<size_t>& task_pt_chain,
    const std::map<std::string, std::vector<std::string>>& task_locks) {
  if (task_pt_chain.size() < TaskChainLayout::kMinLength) {
    spdlog::debug("[TDG2PN] Skip chain for {}: too short for locks", task_name);
    return;
  }

  const auto task_locks_it = task_locks.find(task_name);
  if (task_locks_it == task_locks.end()) {
    spdlog::warn("[TDG2PN] No locks found for task: {}", task_name);
    return;
  }

  const size_t lock_count = task_locks_it->second.size();
  for (size_t lock_index = 0; lock_index < lock_count; ++lock_index) {
    const std::string& lock_type = lock_types[lock_index];

    const size_t acquire_chain_index = TaskChainLayout::lock_acquire_transition(lock_index);
    const size_t release_chain_index =
        TaskChainLayout::lock_release_transition(task_pt_chain.size(), lock_index);

    if (acquire_chain_index >= task_pt_chain.size() ||
        release_chain_index >= task_pt_chain.size()) {
      throw std::runtime_error("Task chain layout does not match lock structure for: " + task_name);
    }

    const size_t acquire_transition = task_pt_chain[acquire_chain_index];
    const size_t release_transition = task_pt_chain[release_chain_index];

    const auto lock_it = ptpn.locks_place.find(lock_type);
    if (lock_it == ptpn.locks_place.end()) {
      throw std::runtime_error("Lock place not found: " + lock_type);
    }

    if (acquire_transition < ptpn.transitions.size()) {
      ptpn.set_pre_arc(lock_it->second, acquire_transition, 1);
    }
    if (release_transition < ptpn.transitions.size()) {
      ptpn.set_post_arc(release_transition, lock_it->second, 1);
    }

    spdlog::debug("[TDG2PN] Bound lock {} to task {}", lock_type, task_name);
  }
}

std::pair<size_t, size_t> TDG2PN::add_node_matrix(petri::PTPN& ptpn, const NodeType& node_type,
                                                  bool resume_mode, int task_place_capacity) {
  return visit_node(node_type, [&](const auto& node) -> std::pair<size_t, size_t> {
    using Node = std::decay_t<decltype(node)>;

    if constexpr (std::is_same_v<Node, TaskNode>) {
      return add_task_node_matrix(ptpn, node, resume_mode, task_place_capacity);
    }

    if constexpr (std::is_same_v<Node, JoinTask>) {
      const petri::TimeInterval interval(node.time.first, node.time.second);
      const size_t join_trans =
          ptpn.add_transition("Join" + std::to_string(ptpn.node_index++), interval, node.priority,
                              node.core, /*suspendable=*/false);
      return {join_trans, join_trans};
    }

    if constexpr (std::is_same_v<Node, ForkTask>) {
      const petri::TimeInterval interval(node.time.first, node.time.second);
      const size_t fork_trans =
          ptpn.add_transition("Fork" + std::to_string(ptpn.node_index++), interval, node.priority,
                              node.core, /*suspendable=*/false);
      return {fork_trans, fork_trans};
    }

    const size_t empty_place = ptpn.add_place("Empty" + std::to_string(ptpn.node_index++), 1);
    return {empty_place, empty_place};
  });
}

std::vector<size_t> TDG2PN::add_execution_chain(petri::PTPN& ptpn, const std::string& task_name,
                                                const std::vector<std::pair<int, int>>& times,
                                                const std::vector<std::string>& locks, int priority,
                                                int core, bool resume_mode,
                                                int task_place_capacity) {
  if (times.empty()) {
    throw std::runtime_error("Task has no execution segments: " + task_name);
  }

  // Task-chain places saturate: producing into a full place never disables the
  // producer, tokens are merged at the capacity bound (single-server semantics,
  // e.g. periodic releases arriving while a previous instance is still queued).
  const int capacity = task_place_capacity;
  const bool saturate = true;

  std::vector<size_t> chain;
  chain.reserve(times.size() * 4 + locks.size() * 2 + 3);

  const size_t entry = ptpn.add_place(task_name + "entry", capacity, saturate);
  const int encoded_priority = encode_task_execution_priority(priority);
  const size_t get_core =
      ptpn.add_transition(task_name + "get_core", immediate_interval(), encoded_priority, core,
                          /*suspendable=*/false);
  const size_t ready = ptpn.add_place(task_name + "ready", capacity, saturate);

  ptpn.set_pre_arc(entry, get_core, 1);
  ptpn.set_post_arc(get_core, ready, 1);
  chain.insert(chain.end(), {entry, get_core, ready});

  size_t current_place = ready;

  for (size_t segment_index = 0; segment_index < times.size(); ++segment_index) {
    const auto& [start, end] = times[segment_index];
    const std::string exec_name = times.size() == 1
                                      ? task_name + "exec"
                                      : task_name + "_exec_" + std::to_string(segment_index + 1);
    // In resume mode the engine models preemption by freezing the execution
    // clock, so execution segments are suspendable. Spin-lock critical sections
    // must keep the CPU, so those segments stay non-suspendable.
    const bool segment_holds_spin_lock =
        segment_index < locks.size() && locks[segment_index].find("spin") != std::string::npos;
    const bool exec_suspendable = resume_mode && !segment_holds_spin_lock;
    const size_t exec = ptpn.add_transition(exec_name, petri::TimeInterval(start, end),
                                            encoded_priority, core, exec_suspendable);

    const bool is_last_segment = segment_index + 1 == times.size();
    const std::string next_place_name =
        is_last_segment ? task_name + "exit"
                        : task_name + "_seg_" + std::to_string(segment_index + 1) + "_done";
    const size_t next_place = ptpn.add_place(next_place_name, capacity, saturate);

    ptpn.set_pre_arc(current_place, exec, 1);
    ptpn.set_post_arc(exec, next_place, 1);
    chain.insert(chain.end(), {exec, next_place});
    current_place = next_place;

    if (segment_index < locks.size()) {
      const std::string lock_name = task_name + "_lock_" + std::to_string(segment_index + 1);
      const size_t lock_transition =
          ptpn.add_transition(lock_name, immediate_interval(), encoded_priority, core,
                              /*suspendable=*/false);
      const size_t hold_place = ptpn.add_place(
          task_name + "_hold_" + std::to_string(segment_index + 1), capacity, saturate);

      ptpn.set_pre_arc(current_place, lock_transition, 1);
      ptpn.set_post_arc(lock_transition, hold_place, 1);
      chain.insert(chain.end(), {lock_transition, hold_place});
      current_place = hold_place;
    }
  }

  return chain;
}

std::pair<size_t, size_t> TDG2PN::add_task_node_matrix(petri::PTPN& ptpn, const TaskNode& task,
                                                       bool resume_mode, int task_place_capacity) {
  std::vector<size_t> chain =
      add_execution_chain(ptpn, task.name, task.time, task.lock, task.priority, task.core,
                          resume_mode, task_place_capacity);
  ptpn.node_pn_map[task.name] = chain;
  return {chain.front(), chain.back()};
}

void TDG2PN::add_monitor_matrix(petri::PTPN& ptpn, const std::string& task_name,
                                int task_period_time, size_t start, size_t end) {
  const size_t deadline = ptpn.add_place(task_name + "deadline", 1);
  const size_t timeout = ptpn.add_place(task_name + "timeout", 1);
  const size_t ok = ptpn.add_place(task_name + "ok", 1);
  const size_t end_place = ptpn.add_place(task_name + "end", 1);

  const size_t timed = add_control_transition(
      ptpn, task_name + "timed", petri::TimeInterval(task_period_time, task_period_time));
  const size_t ending = add_control_transition(ptpn, task_name + "ending");
  const size_t complete = add_control_transition(ptpn, task_name + "complete");
  const size_t timeout_transition = add_control_transition(ptpn, task_name + "out");

  ptpn.set_post_arc(ending, end_place, 1);
  ptpn.set_pre_arc(end_place, complete, 1);
  ptpn.set_post_arc(complete, ok, 1);
  ptpn.set_pre_arc(deadline, ok, 1);
  ptpn.set_pre_arc(deadline, timeout_transition, 1);
  ptpn.set_post_arc(timeout_transition, timeout, 1);

  if (end < ptpn.places.size()) {
    ptpn.set_pre_arc(end, ending, 1);
  } else {
    spdlog::warn("[TDG2PN] end node is transition, cannot add monitor edge");
  }

  if (start < ptpn.places.size()) {
    ptpn.set_pre_arc(start, timed, 1);
    ptpn.set_post_arc(timed, deadline, 1);
  }
}

void TDG2PN::fixed_prior_with_restart(
    petri::PTPN& ptpn, const std::unordered_map<int, std::vector<std::string>>& core_task,
    const std::unordered_map<std::string, TaskConfig>& tc,
    const std::unordered_map<std::string, NodeType>& nodes_type) {
  spdlog::info("[TDG2PN] Starting fixed-priority restart preemption addition");

  auto handle_task_preemption = [&](const std::string& l_t_name, const std::string& h_t_name,
                                    const TaskConfig& l_tc, const TaskConfig& h_tc,
                                    const std::vector<size_t>& l_t_pn,
                                    const std::vector<size_t>& h_t_pn, int preempt_priority,
                                    bool /* is_interrupt */) {
    if (l_t_pn.size() < 5 || h_t_pn.size() < 5) {
      spdlog::warn(
          "[TDG2PN] Task chain too short, skipping restart preemption: {} "
          "<- {}",
          l_t_name, h_t_name);
      return;
    }

    size_t l_exec = l_t_pn[3];
    if (l_exec < ptpn.transitions.size()) {
      ptpn.transitions[l_exec].suspendable = true;
    }

    const size_t l_entry = l_t_pn[0];
    const size_t l_preempt_place = l_t_pn[2];
    const size_t h_entry = h_t_pn[0];
    const size_t h_ready = h_t_pn[2];

    std::string preempt_name =
        h_t_name + "_restart_preempt_" + l_t_name + "_" + std::to_string(ptpn.node_index++);
    petri::TimeInterval preempt_interval(0, 0);
    size_t preempt_trans =
        ptpn.add_transition(preempt_name, preempt_interval, preempt_priority, h_tc.core, false);

    ptpn.set_pre_arc(h_entry, preempt_trans, 1);
    ptpn.set_pre_arc(l_preempt_place, preempt_trans, 1);
    ptpn.set_post_arc(preempt_trans, h_ready, 1);
    ptpn.set_post_arc(preempt_trans, l_entry, 1);

    if (!l_tc.locks.empty()) {
      constexpr size_t MIN_CHAIN_LENGTH = 9;
      if (l_t_pn.size() < MIN_CHAIN_LENGTH) {
        spdlog::debug("[TDG2PN] Task chain too short for lock restart preemption: {}", l_t_name);
        return;
      }

      for (size_t i = 0; i < l_tc.locks.size(); ++i) {
        if (l_tc.locks[i].find("spin") != std::string::npos) {
          break;
        }

        size_t idx = l_t_pn.size() - 2 - 2 * (i + 1);
        if (idx < ptpn.transitions.size()) {
          ptpn.transitions[idx].suspendable = true;
        }

        const size_t lock_preempt_place = l_t_pn[idx - 1];
        std::string lock_preempt_name = h_t_name + "_restart_lock_preempt_" + l_t_name + "_" +
                                        std::to_string(ptpn.node_index++);
        petri::TimeInterval lock_preempt_interval(0, 0);
        size_t lock_preempt_trans = ptpn.add_transition(lock_preempt_name, lock_preempt_interval,
                                                        preempt_priority, h_tc.core, false);

        ptpn.set_pre_arc(h_entry, lock_preempt_trans, 1);
        ptpn.set_pre_arc(lock_preempt_place, lock_preempt_trans, 1);
        ptpn.set_post_arc(lock_preempt_trans, h_ready, 1);
        ptpn.set_post_arc(lock_preempt_trans, l_entry, 1);
      }
    }
  };

  for (const auto& [core_id, tasks] : core_task) {
    spdlog::debug("[TDG2PN] Processing restart preemption for core {}", core_id);

    for (size_t i = 0; i < tasks.size(); ++i) {
      const std::string& h_t_name = tasks[i];
      const auto h_t_it = tc.find(h_t_name);
      if (h_t_it == tc.end()) {
        continue;
      }
      const TaskConfig& h_tc = h_t_it->second;
      const auto preempt_priorities = build_preempt_priorities(tasks, tc, h_tc.priority);

      for (size_t j = i + 1; j < tasks.size(); ++j) {
        const std::string& l_t_name = tasks[j];

        auto l_t_it = tc.find(l_t_name);
        if (l_t_it == tc.end()) {
          continue;
        }
        const auto preempt_priority_it = preempt_priorities.find(l_t_name);
        if (preempt_priority_it == preempt_priorities.end()) {
          continue;
        }

        const TaskConfig& l_tc = l_t_it->second;
        if (l_tc.priority == h_tc.priority) {
          continue;
        }

        auto l_t_pns_it = ptpn.node_pn_map.find(l_t_name);
        auto h_t_pns_it = ptpn.node_pn_map.find(h_t_name);
        if (l_t_pns_it == ptpn.node_pn_map.end() || h_t_pns_it == ptpn.node_pn_map.end()) {
          spdlog::warn("[TDG2PN] Cannot find task chain: {} or {}", l_t_name, h_t_name);
          continue;
        }

        const std::vector<size_t>& h_t_pn = h_t_pns_it->second;
        bool is_interrupt = false;
        auto node_type_it = nodes_type.find(h_t_name);
        if (node_type_it != nodes_type.end() &&
            std::holds_alternative<TaskNode>(node_type_it->second)) {
          const auto& task = std::get<TaskNode>(node_type_it->second);
          is_interrupt = (task.task_type == TaskType::INTERRUPT);
        }

        for (const auto& l_t_pn : {l_t_pns_it->second}) {
          handle_task_preemption(l_t_name, h_t_name, l_tc, h_tc, l_t_pn, h_t_pn,
                                 preempt_priority_it->second, is_interrupt);
        }
      }
    }
  }

  spdlog::info("[TDG2PN] Fixed-priority restart preemption tasks added");
}

}  // namespace converter