#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "model/petri.h"

namespace state_class {

namespace {

bool has_suffix(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Candidate discovery is bounded and deliberately incomplete. In particular an
// infeasible earliest-delay replay does not rule out other timings or words.
constexpr size_t kCandidateLimit = 64;
constexpr size_t kWordLimit = 256;
using LabelledEdges = std::vector<std::vector<std::pair<size_t, int>>>;
using Word = std::vector<std::pair<size_t, int>>;  // target vertex, firing label

struct Path {
  size_t start;
  Word steps;
};

std::optional<Path> shortest_word(const LabelledEdges& edges, const std::vector<size_t>& roots,
                                  size_t goal, const std::vector<long long>* component = nullptr) {
  const size_t none = edges.size();
  std::vector<size_t> parent(none, none);
  std::vector<int> incoming(none);
  std::vector<size_t> distance(none);
  std::deque<size_t> queue;
  for (size_t root : roots) {
    parent[root] = root;
    queue.push_back(root);
  }
  while (!queue.empty() && parent[goal] == none) {
    const size_t source = queue.front();
    queue.pop_front();
    if (distance[source] >= kWordLimit) {
      continue;
    }
    for (const auto& [target, firing] : edges[source]) {
      if (parent[target] != none || (component != nullptr &&
          (*component)[target] != (*component)[goal])) {
        continue;
      }
      parent[target] = source;
      incoming[target] = firing;
      distance[target] = distance[source] + 1;
      queue.push_back(target);
    }
  }
  if (parent[goal] == none) {
    return std::nullopt;
  }
  Path path{goal, {}};
  while (parent[path.start] != path.start) {
    path.steps.emplace_back(path.start, incoming[path.start]);
    path.start = parent[path.start];
  }
  std::reverse(path.steps.begin(), path.steps.end());
  return path;
}

bool same_discrete_state(const StateClass& point, const StateClass& symbolic) {
  return point.marking == symbolic.marking && point.priority_enabled == symbolic.priority_enabled;
}

// Only successor selection uses the symbolic path. Delay, progress, bounds and
// resets are checked from a single concrete valuation, never an edge window.
bool replay_step(const petri::PTPN& net, const StateClassReachabilityGraph& replay,
                 StateClass& point, size_t transition, const StateClass& expected,
                 std::vector<TaskMetrics::PeriodicStep>& trace) {
  if (transition >= net.num_transitions() || !contains(point.priority_enabled, transition) ||
      !point.has_exec_clock(transition)) {
    return false;
  }
  const size_t firing_clock = static_cast<size_t>(point.exec_index(transition));
  const auto& interval = net.get_transition(petri::TransitionId{transition}).time_interval;
  const int firing_age = point.zone.get_constraint(firing_clock, 0);
  if (firing_age < 0 || firing_age == INF_TIME) {
    return false;
  }
  const int delay = std::max(0, interval.earliest - firing_age);
  std::vector<int> age(point.clock_vars.size(), 0);
  for (size_t i = 1; i < age.size(); ++i) {
    const ClockVar& clock = point.clock_vars[i];
    const int current = point.zone.get_constraint(i, 0);
    if (clock.kind != ClockKind::Execution || current < 0 || current == INF_TIME ||
        point.zone.get_constraint(0, i) != -current) {
      return false;
    }
    const bool active = contains(point.priority_enabled, clock.transition);
    const long long shifted = static_cast<long long>(current) + (active ? delay : 0);
    const int latest = net.get_transition(petri::TransitionId{clock.transition}).time_interval.latest;
    if (shifted >= INF_TIME || (active && shifted > latest)) {
      return false;
    }
    age[i] = static_cast<int>(shifted);
  }
  for (size_t i = 0; i < age.size(); ++i) {
    for (size_t j = 0; j < age.size(); ++j) {
      point.zone.set_constraint(i, j, age[i] - age[j]);
    }
  }
  for (auto& successor : replay.fire_all(point, transition)) {
    if (same_discrete_state(successor, expected)) {
      point = std::move(successor);
      trace.push_back({transition, net.get_transition(petri::TransitionId{transition}).name,
                       delay, point.priority_enabled});
      return true;
    }
  }
  return false;
}

std::optional<TaskMetrics::PeriodicCertificate> replay_cycle(
    const petri::PTPN& net, StateClassReachabilityGraph& replay, const Path& prefix,
    const Word& cycle, const std::vector<const StateClass*>& states) {
  for (StateClass point : replay.compute_initial_classes()) {
    if (!same_discrete_state(point, *states[prefix.start])) {
      continue;
    }
    TaskMetrics::PeriodicCertificate result;
    result.initial_active = point.priority_enabled;
    const auto execute = [&](const Word& word, auto& trace) {
      for (const auto& [target, firing] : word) {
        if (!replay_step(net, replay, point, static_cast<size_t>(firing), *states[target], trace)) {
          return false;
        }
      }
      return true;
    };
    if (!execute(prefix.steps, result.prefix)) {
      continue;
    }
    const StateClass anchor = point;
    if (!execute(cycle, result.cycle) || !same_discrete_state(point, anchor) ||
        point.clock_vars != anchor.clock_vars || !(point.zone == anchor.zone)) {
      continue;
    }
    for (const auto& step : result.cycle) {
      result.cycle_duration += step.delay;
    }
    if (result.cycle_duration > 0) {
      return result;
    }
  }
  return std::nullopt;
}

}  // namespace

// Iterative Tarjan SCC over a masked subgraph (declared in metrics_internal.h
// and shared with the steady-cycle analysis).
std::vector<long long> tarjan_components(const std::vector<std::vector<size_t>>& adjacency,
                                         const std::vector<char>& in_subgraph) {
  const size_t n = adjacency.size();
  std::vector<long long> index(n, -1);
  std::vector<long long> lowlink(n, 0);
  std::vector<long long> component(n, -1);
  std::vector<char> on_stack(n, 0);
  std::vector<size_t> tarjan_stack;
  long long next_index = 0;
  long long component_count = 0;

  struct Frame {
    size_t vertex;
    size_t next_edge;
  };

  std::vector<Frame> call_stack;

  for (size_t root = 0; root < n; ++root) {
    if (!in_subgraph[root] || index[root] >= 0) {
      continue;
    }

    call_stack.push_back({root, 0});
    index[root] = lowlink[root] = next_index++;
    tarjan_stack.push_back(root);
    on_stack[root] = 1;

    while (!call_stack.empty()) {
      Frame& frame = call_stack.back();
      const size_t v = frame.vertex;

      if (frame.next_edge < adjacency[v].size()) {
        const size_t w = adjacency[v][frame.next_edge++];
        if (index[w] < 0) {
          index[w] = lowlink[w] = next_index++;
          tarjan_stack.push_back(w);
          on_stack[w] = 1;
          call_stack.push_back({w, 0});
        } else if (on_stack[w]) {
          lowlink[v] = std::min(lowlink[v], index[w]);
        }
        continue;
      }

      call_stack.pop_back();
      if (!call_stack.empty()) {
        const size_t parent = call_stack.back().vertex;
        lowlink[parent] = std::min(lowlink[parent], lowlink[v]);
      }
      if (lowlink[v] == index[v]) {
        while (true) {
          const size_t w = tarjan_stack.back();
          tarjan_stack.pop_back();
          on_stack[w] = 0;
          component[w] = component_count;
          if (w == v) {
            break;
          }
        }
        ++component_count;
      }
    }
  }

  return component;
}

void MetricsAnalyzer::compute_local_deadlocks(MetricsReport& report) {
  LabelledEdges prefix_edges;
  std::vector<size_t> initials;
  std::optional<StateClassReachabilityGraph> replay;
  if (periodic_certification_) {
    const SchedulePolicy policy = net_.schedule_policy();
    if ((policy != SchedulePolicy::FIXED && policy != SchedulePolicy::FIXED_PRIOR_WITH_RESUME) ||
        net_.periodic_transitions().empty() ||
        std::any_of(net_.transitions().begin(), net_.transitions().end(), [](const auto& t) {
          return t.time_interval.left_open || t.time_interval.right_open;
        })) {
      return;
    }
    prefix_edges.resize(num_vertices_);
    for (size_t v = 0; v < num_vertices_; ++v) {
      for (const auto& edge : out_edges_[v]) {
        prefix_edges[v].emplace_back(edge.target, edge.transition_id);
      }
    }
    for (auto initial : initial_vertices_) {
      initials.push_back(boost::get(boost::vertex_name, graph_, initial).id);
    }
    replay.emplace(net_);  // only execution clocks constrain this supported policy
  }
  // Periodic-release transitions. TDG lowering records them explicitly; a
  // hand-written .ptpn has no metadata, so fall back to the lowering naming
  // convention (<task>_fire).
  std::unordered_set<size_t> all_periodic;
  for (const auto& [task, transition] : net_.periodic_transitions()) {
    (void)task;
    all_periodic.insert(transition.index());
  }
  if (all_periodic.empty()) {
    for (size_t t = 0; t < net_.num_transitions(); ++t) {
      const auto& transition = net_.get_transition(petri::TransitionId{t});
      if (has_suffix(transition.name, "_fire")) {
        all_periodic.insert(t);
      }
    }
  }

  for (size_t task_index = 0; task_index < report.tasks.size(); ++task_index) {
    TaskMetrics& metrics = report.tasks[task_index];
    if (task_index >= tasks_.size()) {
      continue;
    }
    const TaskTopology& task = tasks_[task_index];

    // In certification mode all loop vertices must represent the same one
    // in-flight job. A release/completion, or an outside transition modifying
    // that chain, makes the word unsuitable as a certificate.
    std::vector<char> preserves_job;
    if (periodic_certification_) {
      preserves_job.assign(net_.num_transitions(), 1);
      const auto touches_chain = [&](const auto& arcs) {
        return std::any_of(arcs.begin(), arcs.end(), [&](const auto& arc) {
          return std::find(task.chain_places.begin(), task.chain_places.end(), arc.first) !=
                 task.chain_places.end();
        });
      };
      for (size_t t = 0; t < net_.num_transitions(); ++t) {
        const auto& post = net_.post_arcs()[t];
        const bool replaces_job = std::any_of(post.begin(), post.end(), [&](const auto& arc) {
          return arc.first == task.entry_place || arc.first == task.end_place;
        });
        const bool inside = std::find(task.chain_transitions.begin(), task.chain_transitions.end(),
                                      t) != task.chain_transitions.end();
        preserves_job[t] = !replaces_job && (inside || (!touches_chain(net_.pre_arcs()[t]) &&
                                                        !touches_chain(post)));
      }
    }

    // T_p(a): periodic transitions that release a new instance of this task.
    std::unordered_set<size_t> periodic_of_task;
    const auto metadata_it = net_.periodic_transitions().find(task.name);
    if (metadata_it != net_.periodic_transitions().end()) {
      periodic_of_task.insert(metadata_it->second.index());
    } else {
      for (size_t t : all_periodic) {
        if (net_.get_transition(petri::TransitionId{t}).name == task.name + "_fire") {
          periodic_of_task.insert(t);
        }
      }
    }

    // Omega_a^pend: the current instance is still before its exit place.
    std::vector<char> pending(num_vertices_, 0);
    bool any_pending = false;
    for (size_t v = 0; v < num_vertices_; ++v) {
      pending[v] = task_pending(task, v) ? 1 : 0;
      if (pending[v] && periodic_certification_) {
        long long jobs = 0;
        for (size_t place : task.chain_places) {
          jobs += state_of_[v]->marking[place];
        }
        pending[v] = jobs == 1 && state_of_[v]->marking[task.end_place] == 0;
      }
      any_pending = any_pending || pending[v];
    }
    if (!any_pending) {
      continue;
    }

    // Delta_a^pend: keep internal edges, except those labelled by T_p(a)
    // (they release the next instance and end the current analysis window).
    std::vector<std::vector<size_t>> adjacency(num_vertices_);
    std::vector<std::vector<std::pair<size_t, int>>> edges_with_labels(num_vertices_);
    for (size_t v = 0; v < num_vertices_; ++v) {
      if (!pending[v]) {
        continue;
      }
      for (const Edge& e : out_edges_[v]) {
        if (!pending[e.target]) {
          continue;
        }
        if (periodic_of_task.count(static_cast<size_t>(e.transition_id)) > 0) {
          continue;
        }
        if (periodic_certification_ && (static_cast<size_t>(e.transition_id) >= preserves_job.size() ||
                                       !preserves_job[static_cast<size_t>(e.transition_id)])) {
          continue;
        }
        adjacency[v].push_back(e.target);
        edges_with_labels[v].push_back({e.target, e.transition_id});
      }
    }

    const std::vector<long long> component = tarjan_components(adjacency, pending);

    // Component sizes and cyclic flags.
    std::vector<size_t> component_size;
    std::vector<char> component_cyclic;
    for (size_t v = 0; v < num_vertices_; ++v) {
      if (component[v] < 0) {
        continue;
      }
      const size_t c = static_cast<size_t>(component[v]);
      if (component_size.size() <= c) {
        component_size.resize(c + 1, 0);
        component_cyclic.resize(c + 1, 0);
      }
      component_size[c]++;
    }
    for (size_t v = 0; v < num_vertices_; ++v) {
      if (component[v] < 0) {
        continue;
      }
      const size_t c = static_cast<size_t>(component[v]);
      for (const auto& [target, transition] : edges_with_labels[v]) {
        (void)transition;
        if (component[target] == component[v]) {
          component_cyclic[c] = 1;  // self-loop or internal edge
          break;
        }
      }
    }

    // A cyclic SCC is a periodic local deadlock when it contains an internal
    // edge labelled by a positive-periodic transition of *another* task.
    std::vector<char> progress_component(component_size.size(), 0);
    size_t attempts = 0;
    for (size_t v = 0; v < num_vertices_; ++v) {
      if (component[v] < 0) {
        continue;
      }
      const size_t c = static_cast<size_t>(component[v]);
      for (const auto& [target, transition_id] : edges_with_labels[v]) {
        if (component[target] != component[v]) {
          continue;
        }
        const size_t t = static_cast<size_t>(transition_id);
        if (t >= net_.num_transitions() || all_periodic.count(t) == 0 ||
            periodic_of_task.count(t) > 0) {
          continue;
        }
        if (net_.get_transition(petri::TransitionId{t}).time_interval.effective_earliest() > 0) {
          if (!periodic_certification_) {
            progress_component[c] = 1;
            break;
          }
          if (attempts++ == kCandidateLimit) {
            break;
          }
          const auto prefix = shortest_word(prefix_edges, initials, v);
          const auto tail = shortest_word(edges_with_labels, {target}, v, &component);
          if (prefix && tail) {
            Word cycle{{target, transition_id}};
            cycle.insert(cycle.end(), tail->steps.begin(), tail->steps.end());
            metrics.periodic_certificate = replay_cycle(net_, *replay, *prefix, cycle, state_of_);
            if (metrics.periodic_certificate) {
              progress_component[c] = 1;
              break;
            }
          }
        }
      }
      if (metrics.periodic_certificate || attempts >= kCandidateLimit) {
        break;
      }
    }

    for (size_t c = 0; c < component_size.size(); ++c) {
      const bool cyclic = component_size[c] > 1 || component_cyclic[c] != 0;
      if (cyclic && progress_component[c] != 0) {
        metrics.local_deadlock = true;
        metrics.local_deadlock_scc_size =
            std::max(metrics.local_deadlock_scc_size, component_size[c]);
      }
    }
  }
}

}  // namespace state_class
