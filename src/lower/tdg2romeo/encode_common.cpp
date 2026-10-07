#include "encode_common.h"

#include <set>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace romeo {

namespace {

RomeoTransition make_transition(const std::string& name, const RomeoTimeInterval& interval,
                                std::optional<int> priority, const std::string& when_guard,
                                std::vector<RomeoAssignment> intermediate,
                                std::vector<RomeoAssignment> updates,
                                std::optional<std::string> allow = std::nullopt) {
  RomeoTransition transition;
  transition.name = name;
  transition.interval = interval;
  transition.priority = priority;
  transition.when_guard = when_guard;
  transition.intermediate = std::move(intermediate);
  transition.updates = std::move(updates);
  transition.allow = std::move(allow);
  return transition;
}

void add_bridge_transition(EncodeContext& ctx, const std::string& source_exit,
                           const std::string& target_entry, const std::string& name,
                           const RomeoTimeInterval& interval) {
  std::vector<std::string> guard_clauses = {guard_ge(source_exit)};
  ctx.builder.add_transition(make_transition(name, interval, kControlPriority,
                                             guard_and(guard_clauses), {{source_exit, -1}},
                                             {{source_exit, -1}, {target_entry, +1}}));
}

}  // namespace

void add_core_places(EncodeContext& ctx, const tdg::TDG& tdg) {
  if (!ctx.explicit_core_places) {
    return;
  }
  for (int core = 0; core < tdg.num_cpus; ++core) {
    ctx.builder.place(core_place_name(core), tdg.cores_per_cpu);
  }
}

void add_lock_places(EncodeContext& ctx, const tdg::TDG& tdg) {
  for (const auto& lock_name : tdg.lock_set) {
    ctx.builder.place(lock_name, 1);
  }
}

void add_task_chain_scheduling_net(EncodeContext& ctx, const TaskNode& task) {
  const std::string entry = place_name(task.name, "entry");
  const std::string ready = place_name(task.name, "ready");
  const std::string exit = place_name(task.name, "exit");

  ctx.builder.place(entry, 0);
  ctx.builder.place(ready, 0);
  ctx.builder.place(exit, 0);

  std::vector<std::string> get_core_guard = {guard_ge(entry)};
  std::vector<RomeoAssignment> get_core_intermediate = {{entry, -1}};
  if (ctx.explicit_core_places) {
    const std::string core = core_place_name(task.core);
    get_core_guard.push_back(guard_ge(core));
    get_core_intermediate.push_back({core, -1});
  }

  std::vector<RomeoAssignment> get_core_updates = {{entry, -1}, {ready, +1}};
  ctx.builder.add_transition(make_transition(task.name + "get_core", RomeoTimeInterval::immediate(),
                                             task.priority, guard_and(get_core_guard),
                                             get_core_intermediate, get_core_updates));

  std::string current_place = ready;
  const size_t segment_count = task.time.size();
  for (size_t segment_index = 0; segment_index < segment_count; ++segment_index) {
    const std::string exec_name = segment_count == 1
                                      ? task.name + "exec"
                                      : task.name + "_exec_" + std::to_string(segment_index + 1);
    const bool is_last = segment_index + 1 == segment_count;
    const std::string next_place =
        is_last ? exit
                : place_name(task.name, "_seg_" + std::to_string(segment_index + 1) + "_done");

    if (!is_last) {
      ctx.builder.place(next_place, 0);
    }

    std::vector<RomeoAssignment> exec_updates = {{current_place, -1}, {next_place, +1}};
    if (is_last && ctx.explicit_core_places) {
      exec_updates.push_back({core_place_name(task.core), +1});
    }
    for (size_t lock_index = 0; lock_index < task.lock.size(); ++lock_index) {
      if (segment_index == lock_index + 1) {
        exec_updates.push_back({task.lock[lock_index], +1});
      }
    }

    ctx.builder.add_transition(make_transition(
        exec_name,
        RomeoTimeInterval::closed(task.time[segment_index].first, task.time[segment_index].second),
        task.priority, guard_ge(current_place), {{current_place, -1}}, exec_updates));

    current_place = next_place;

    if (segment_index < task.lock.size()) {
      const std::string& lock_type = task.lock[segment_index];
      const std::string hold = place_name(task.name, "_hold_" + std::to_string(segment_index + 1));
      ctx.builder.place(hold, 0);

      std::vector<std::string> lock_guard = {guard_ge(current_place), guard_ge(lock_type)};
      ctx.builder.add_transition(make_transition(
          task.name + "_lock_" + std::to_string(segment_index + 1), RomeoTimeInterval::immediate(),
          task.priority, guard_and(lock_guard), {{current_place, -1}, {lock_type, -1}},
          {{current_place, -1}, {hold, +1}}));

      current_place = hold;
    }
  }

  ctx.tasks[task.name] = {entry, exit, task.core, task.priority, ""};
  ctx.nodes[task.name] = {entry, exit, false};
}

void add_task_chain_inhibitor_arc(EncodeContext& ctx, const tdg::TDG& tdg, const TaskNode& task) {
  const std::string entry = place_name(task.name, "entry");
  const std::string ready = place_name(task.name, "ready");
  const std::string exit = place_name(task.name, "exit");
  const std::string active = place_name(task.name, "active");

  ctx.builder.place(entry, 0);
  ctx.builder.place(ready, 0);
  ctx.builder.place(exit, 0);
  ctx.builder.place(active, 0);

  if (ctx.explicit_core_places) {
    ctx.builder.place(core_busy_place_name(task.core), 0);
  }

  std::vector<std::string> allow_parts;
  if (ctx.explicit_core_places) {
    allow_parts.push_back(core_busy_place_name(task.core) + " == 0");
  }
  const auto by_core = group_tasks_by_core(tdg);
  const auto core_it = by_core.find(task.core);
  if (core_it != by_core.end()) {
    for (const auto& higher_task : core_it->second) {
      if (higher_task == task.name) {
        break;
      }
      allow_parts.push_back(place_name(higher_task, "active") + " == 0");
    }
  }

  std::vector<std::string> sched_guard = {guard_ge(entry)};
  const std::optional<std::string> allow_expr =
      allow_parts.empty() ? std::nullopt : std::optional<std::string>(guard_and(allow_parts));

  ctx.builder.add_transition(make_transition(task.name + "sched", RomeoTimeInterval::immediate(),
                                             task.priority, guard_and(sched_guard), {{entry, -1}},
                                             {{entry, -1}, {ready, +1}, {active, +1}}, allow_expr));

  std::string current_place = ready;
  const size_t segment_count = task.time.size();
  for (size_t segment_index = 0; segment_index < segment_count; ++segment_index) {
    const bool is_last = segment_index + 1 == segment_count;
    const std::string exec_name = segment_count == 1
                                      ? task.name + "exec"
                                      : task.name + "_exec_" + std::to_string(segment_index + 1);
    const std::string next_place =
        is_last ? exit
                : place_name(task.name, "_seg_" + std::to_string(segment_index + 1) + "_done");

    if (!is_last) {
      ctx.builder.place(next_place, 0);
    }

    std::vector<std::string> exec_guard = {guard_ge(current_place)};
    std::vector<RomeoAssignment> exec_intermediate = {{current_place, -1}};
    std::vector<RomeoAssignment> exec_updates = {{current_place, -1}, {next_place, +1}};

    if (segment_index == 0 && ctx.explicit_core_places) {
      exec_guard.push_back(core_busy_place_name(task.core) + " == 0");
      exec_updates.push_back({core_busy_place_name(task.core), +1});
    }
    if (is_last) {
      if (ctx.explicit_core_places) {
        exec_updates.push_back({core_busy_place_name(task.core), -1});
      }
      exec_updates.push_back({active, -1});
    }
    for (size_t lock_index = 0; lock_index < task.lock.size(); ++lock_index) {
      if (segment_index == lock_index + 1) {
        exec_updates.push_back({task.lock[lock_index], +1});
      }
    }

    ctx.builder.add_transition(make_transition(
        exec_name,
        RomeoTimeInterval::closed(task.time[segment_index].first, task.time[segment_index].second),
        std::nullopt, guard_and(exec_guard), exec_intermediate, exec_updates));

    current_place = next_place;

    if (segment_index < task.lock.size()) {
      const std::string& lock_type = task.lock[segment_index];
      const std::string hold = place_name(task.name, "_hold_" + std::to_string(segment_index + 1));
      ctx.builder.place(hold, 0);

      ctx.builder.add_transition(make_transition(
          task.name + "_lock_" + std::to_string(segment_index + 1), RomeoTimeInterval::immediate(),
          std::nullopt, guard_and({guard_ge(current_place), guard_ge(lock_type)}),
          {{current_place, -1}, {lock_type, -1}}, {{current_place, -1}, {hold, +1}}));

      current_place = hold;
    }
  }

  ctx.tasks[task.name] = {entry, exit, task.core, task.priority, active};
  ctx.nodes[task.name] = {entry, exit, false};
}

void add_fork_join_nodes(EncodeContext& ctx, const tdg::TDG& tdg) {
  for (const auto& [name, node_type] : tdg.nodes_type) {
    if (std::holds_alternative<ForkTask>(node_type)) {
      const auto& fork = std::get<ForkTask>(node_type);
      const std::string out_place = name + "_out";
      ctx.builder.place(out_place, 0);
      ctx.builder.add_transition(
          make_transition(name, RomeoTimeInterval::closed(fork.time.first, fork.time.second),
                          fork.priority, "true", {}, {{out_place, +1}}));
      ctx.nodes[name] = {out_place, out_place, true};
    } else if (std::holds_alternative<JoinTask>(node_type)) {
      const auto& join = std::get<JoinTask>(node_type);
      const std::string in_place = name + "_in";
      ctx.builder.place(in_place, 0);
      ctx.builder.add_transition(
          make_transition(name, RomeoTimeInterval::closed(join.time.first, join.time.second),
                          join.priority, guard_ge(in_place), {{in_place, -1}}, {{in_place, -1}}));
      ctx.nodes[name] = {in_place, in_place, true};
    }
  }
}

void wire_edges(EncodeContext& ctx, const tdg::TDG& tdg) {
  for (const auto& edge : tdg.tdg_edges) {
    if (edge.is_self_loop() || edge.is_dashed()) {
      continue;
    }

    const auto source_it = ctx.nodes.find(edge.source);
    const auto target_it = ctx.nodes.find(edge.target);
    if (source_it == ctx.nodes.end() || target_it == ctx.nodes.end()) {
      throw std::runtime_error("Unknown edge endpoint: " + edge.source + " -> " + edge.target);
    }

    const auto source_type = tdg.nodes_type.find(edge.source);
    const auto target_type = tdg.nodes_type.find(edge.target);
    if (source_type == tdg.nodes_type.end() || target_type == tdg.nodes_type.end()) {
      throw std::runtime_error("Missing node type for edge: " + edge.source + " -> " + edge.target);
    }

    const bool source_is_control = is_fork_or_join(source_type->second);
    const bool target_is_control = is_fork_or_join(target_type->second);

    if (source_is_control && target_is_control) {
      throw std::runtime_error("Unsupported edge between control nodes: " + edge.source + " -> " +
                               edge.target);
    }

    if (source_is_control) {
      add_bridge_transition(ctx, source_it->second.exit, target_it->second.entry,
                            edge.source + "_to_" + edge.target, RomeoTimeInterval::immediate());
      continue;
    }

    if (target_is_control) {
      add_bridge_transition(ctx, source_it->second.exit, target_it->second.entry,
                            edge.source + "_to_" + edge.target, RomeoTimeInterval::immediate());
      continue;
    }

    const RomeoTimeInterval interval = parse_edge_interval(edge.label, edge.source, edge.target);
    add_bridge_transition(ctx, source_it->second.exit, target_it->second.entry,
                          edge.source + "_to_" + edge.target, interval);
  }
}

void add_start_tokens(EncodeContext& ctx, const tdg::TDG& tdg) {
  for (const auto& start : tdg.start_tasks) {
    const auto node_it = ctx.nodes.find(start.task);
    if (node_it == ctx.nodes.end()) {
      spdlog::warn("[TDG2ROMEO] Start task not found: {}", start.task);
      continue;
    }
    if (start.tokens > 0) {
      ctx.builder.place(node_it->second.entry, start.tokens);
    }
  }
}

void add_periodic_releases(EncodeContext& ctx, const tdg::TDG& tdg) {
  for (const auto& periodic : tdg.periodic_tasks) {
    if (has_self_loop_release(tdg, periodic.task)) {
      continue;
    }
    const auto node_it = ctx.nodes.find(periodic.task);
    if (node_it == ctx.nodes.end()) {
      spdlog::warn("[TDG2ROMEO] Periodic task not found: {}", periodic.task);
      continue;
    }

    const std::string period_place = periodic.task + "_period";
    ctx.builder.place(period_place, 1);
    ctx.builder.add_transition(
        make_transition(periodic.task + "_fire", RomeoTimeInterval::point(periodic.period),
                        kControlPriority, guard_ge(period_place), {{period_place, -1}},
                        {{node_it->second.entry, +1}, {period_place, +1}}));
  }
}

void add_end_consumers(EncodeContext& ctx, const tdg::TDG& tdg) {
  std::set<std::string> consume_tasks(tdg.end_tasks.begin(), tdg.end_tasks.end());
  for (const auto& [name, node_type] : tdg.nodes_type) {
    if (as_task_node(node_type) && !has_non_self_successor(tdg, name)) {
      consume_tasks.insert(name);
    }
  }

  for (const auto& task_name : consume_tasks) {
    const auto node_it = ctx.nodes.find(task_name);
    if (node_it == ctx.nodes.end()) {
      continue;
    }
    ctx.builder.add_transition(
        make_transition(task_name + "_consume", RomeoTimeInterval::immediate(), kControlPriority,
                        guard_ge(node_it->second.exit), {{node_it->second.exit, -1}},
                        {{node_it->second.exit, -1}}));
  }
}

}  // namespace romeo
